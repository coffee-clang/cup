#!/bin/sh

# Verifies the exact metadata produced by a native source-test job.
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
PROJECT_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/../.." && pwd -P)
. "$SCRIPT_DIR/evidence-common.sh"

fail() {
    printf 'source evidence: %s\n' "$*" >&2
    exit 1
}

usage() {
    printf '%s\n' \
        "Usage: $0 <directory> <platform> <repository> <commit> <run-id>" \
        '       <run-attempt> <artifact-name> [candidate-config]' >&2
    exit 2
}

parse_arguments() {
    case "$#" in
        7|8)
            EVIDENCE_DIR=$1
            EXPECTED_PLATFORM=$2
            EXPECTED_REPOSITORY=$3
            EXPECTED_COMMIT=$4
            EXPECTED_RUN_ID=$5
            EXPECTED_RUN_ATTEMPT=$6
            EXPECTED_ARTIFACT_NAME=$7
            CANDIDATE_CONFIG=${8:-}
            ;;
        *)
            usage
            ;;
    esac
}

require_canonical_text() {
    ci_evidence_require_canonical_text "$1" "${2:-4194304}" ||
        fail "evidence is not canonical: $1"
}

exact_field() {
    file=$1
    key=$2
    count=$(awk -F= -v key="$key" '$1 == key { count++ } END { print count + 0 }' "$file")

    [ "$count" -eq 1 ] || fail "$file must contain exactly one $key field"
    awk -F= -v key="$key" '$1 == key { sub(/^[^=]*=/, ""); print }' "$file"
}

require_value() {
    label=$1
    actual=$2
    expected=$3

    [ "$actual" = "$expected" ] ||
        fail "$label '$actual' does not match '$expected'"
}

require_nonempty() {
    label=$1
    value=$2

    [ -n "$value" ] && [ "$value" != missing ] ||
        fail "$label is empty or unavailable"
}

require_sha256() {
    label=$1
    value=$2

    [ "${#value}" -eq 64 ] || fail "$label is not 64 hexadecimal characters"
    case "$value" in
        *[!0-9a-f]*)
            fail "$label is not a lowercase SHA-256 value"
            ;;
    esac
}

resolve_and_validate_inputs() {
    EXPECTED_VERSION=$("$PROJECT_ROOT/scripts/version.sh" base)

    EVIDENCE_DIR=$(ci_evidence_resolve_path "$(pwd -P)" "$EVIDENCE_DIR")
    if [ -n "$CANDIDATE_CONFIG" ]; then
        CANDIDATE_CONFIG=$(ci_evidence_resolve_path "$(pwd -P)" "$CANDIDATE_CONFIG")
    fi

    ci_evidence_validate_slug "$EXPECTED_PLATFORM" platform ||
        fail 'invalid expected platform'
    ci_evidence_validate_repository "$EXPECTED_REPOSITORY" ||
        fail 'invalid expected repository'
    ci_evidence_validate_sha "$EXPECTED_COMMIT" || fail 'invalid expected commit'
    ci_evidence_validate_number "$EXPECTED_RUN_ID" || fail 'invalid expected run ID'
    ci_evidence_validate_number "$EXPECTED_RUN_ATTEMPT" ||
        fail 'invalid expected run attempt'
    ci_evidence_validate_artifact_name "$EXPECTED_ARTIFACT_NAME" ||
        fail 'invalid expected artifact name'
}

validate_evidence_directory() {
    cup_path_check_directory_chain "$EVIDENCE_DIR" 0 'source evidence directory' ||
        fail "unsafe evidence directory: $EVIDENCE_DIR"
    cup_path_require_safe_tree "$EVIDENCE_DIR" 'source evidence directory' ||
        fail 'evidence contains links or special files'

    expected_names=$(printf '%s\n' \
        binary-inspection.txt build-config.txt evidence.txt release.txt)
    actual_names=$(
        for entry in "$EVIDENCE_DIR"/* "$EVIDENCE_DIR"/.[!.]* "$EVIDENCE_DIR"/..?*; do
            [ -e "$entry" ] || [ -L "$entry" ] || continue
            basename -- "$entry"
        done | LC_ALL=C sort
    )
    if [ "$actual_names" != "$expected_names" ]; then
        printf 'source evidence: expected files:\n%s\n' "$expected_names" >&2
        printf 'source evidence: actual files:\n%s\n' "$actual_names" >&2
        exit 1
    fi

    BUILD_CONFIG=$EVIDENCE_DIR/build-config.txt
    RELEASE=$EVIDENCE_DIR/release.txt
    INSPECTION=$EVIDENCE_DIR/binary-inspection.txt
    ENVELOPE=$EVIDENCE_DIR/evidence.txt

    for file in "$BUILD_CONFIG" "$RELEASE" "$INSPECTION" "$ENVELOPE"; do
        cup_path_require_regular_file "$file" 'source evidence file' ||
            fail "unsafe evidence file: $file"
        require_canonical_text "$file"
    done
}

verify_envelope() {
    envelope_expected=$(cat <<EOF_ENVELOPE
format=1
version=$EXPECTED_VERSION
source_repository=$EXPECTED_REPOSITORY
source_commit=$EXPECTED_COMMIT
run_id=$EXPECTED_RUN_ID
run_attempt=$EXPECTED_RUN_ATTEMPT
artifact_name=$EXPECTED_ARTIFACT_NAME
platform=$EXPECTED_PLATFORM
build_config_sha256=$(ci_evidence_sha256_file "$BUILD_CONFIG")
release_sha256=$(ci_evidence_sha256_file "$RELEASE")
binary_inspection_sha256=$(ci_evidence_sha256_file "$INSPECTION")
EOF_ENVELOPE
)

    [ "$(cat "$ENVELOPE")" = "$envelope_expected" ] ||
        fail 'source evidence envelope or artifact identity does not match'
}

load_source_build_contract() {
    BUILD_KEYS='format
platform
configuration
host_system
host_machine
compiler_command
compiler_path
compiler_target
compiler_target_normalized
compiler_version
compiler_numeric
windres_command
windres_path
windres_version
windres_numeric
windres_target_normalized
cppflags
cflags
ldflags
ldlibs
deps_prefix
dependency_prefix_format
dependency_platform
dependency_profile
dependency_build_revision
dependency_source_lock_sha256
dependency_toolchain_sha256
official_build'

    actual_build_keys=$(sed 's/=.*//' "$BUILD_CONFIG")
    [ "$actual_build_keys" = "$BUILD_KEYS" ] ||
        fail 'build-config.txt has an unexpected schema or key order'

    require_value build-config.format "$(exact_field "$BUILD_CONFIG" format)" 3
    require_value build-config.platform \
        "$(exact_field "$BUILD_CONFIG" platform)" "$EXPECTED_PLATFORM"
    require_value build-config.configuration \
        "$(exact_field "$BUILD_CONFIG" configuration)" development
    require_value build-config.official_build \
        "$(exact_field "$BUILD_CONFIG" official_build)" 0
    require_value build-config.dependency_platform \
        "$(exact_field "$BUILD_CONFIG" dependency_platform)" "$EXPECTED_PLATFORM"

    source_lock=$(exact_field "$BUILD_CONFIG" dependency_source_lock_sha256)
    source_toolchain=$(exact_field "$BUILD_CONFIG" dependency_toolchain_sha256)
    require_sha256 build-config.dependency_source_lock_sha256 "$source_lock"
    require_sha256 build-config.dependency_toolchain_sha256 "$source_toolchain"

    source_prefix_format=$(exact_field "$BUILD_CONFIG" dependency_prefix_format)
    source_profile=$(exact_field "$BUILD_CONFIG" dependency_profile)
    source_revision=$(exact_field "$BUILD_CONFIG" dependency_build_revision)
    source_prefix=$(exact_field "$BUILD_CONFIG" deps_prefix)
    cup_path_validate_absolute_clean "$source_prefix" 'source dependency prefix' ||
        fail 'source dependency prefix is not an absolute clean path'

    source_compiler_command=$(exact_field "$BUILD_CONFIG" compiler_command)
    source_compiler_path=$(exact_field "$BUILD_CONFIG" compiler_path)
    source_compiler_target=$(exact_field "$BUILD_CONFIG" compiler_target)
    source_compiler_target_normalized=$(exact_field "$BUILD_CONFIG" compiler_target_normalized)
    source_compiler_version=$(exact_field "$BUILD_CONFIG" compiler_version)
    source_compiler_numeric=$(exact_field "$BUILD_CONFIG" compiler_numeric)
    require_nonempty build-config.compiler_command "$source_compiler_command"
    require_nonempty build-config.compiler_path "$source_compiler_path"
    require_nonempty build-config.compiler_target "$source_compiler_target"
    require_nonempty build-config.compiler_target_normalized "$source_compiler_target_normalized"
    require_value build-config.compiler_target_normalized "$source_compiler_target_normalized" "$EXPECTED_PLATFORM"
    require_nonempty build-config.compiler_version "$source_compiler_version"
    require_nonempty build-config.compiler_numeric "$source_compiler_numeric"
    cup_path_validate_absolute_clean "$source_compiler_path" 'source compiler path' ||
        fail 'source compiler path is not an absolute clean path'

    source_windres_command=$(exact_field "$BUILD_CONFIG" windres_command)
    source_windres_path=$(exact_field "$BUILD_CONFIG" windres_path)
    source_windres_version=$(exact_field "$BUILD_CONFIG" windres_version)
    source_windres_numeric=$(exact_field "$BUILD_CONFIG" windres_numeric)
    source_windres_target_normalized=$(exact_field "$BUILD_CONFIG" windres_target_normalized)
    if [ "$EXPECTED_PLATFORM" = windows-x64 ]; then
        require_nonempty build-config.windres_command "$source_windres_command"
        require_nonempty build-config.windres_path "$source_windres_path"
        require_nonempty build-config.windres_version "$source_windres_version"
        require_nonempty build-config.windres_numeric "$source_windres_numeric"
        require_value build-config.windres_target_normalized "$source_windres_target_normalized" windows-x64
        cup_path_validate_absolute_clean "$source_windres_path" 'source windres path' ||
            fail 'source windres path is not an absolute clean path'
    elif [ -n "$source_windres_command" ] || [ -n "$source_windres_path" ] ||
        [ -n "$source_windres_version" ] || [ -n "$source_windres_numeric" ] ||
        [ -n "$source_windres_target_normalized" ]; then
        fail 'non-Windows source build contains windres identity'
    fi
}

verify_release_metadata() {
    release_expected=$(printf 'format=1\nversion=%s\ncommit=%s' \
        "$EXPECTED_VERSION" "$EXPECTED_COMMIT")
    [ "$(cat "$RELEASE")" = "$release_expected" ] ||
        fail 'release.txt does not match the expected version and commit'
}

select_inspection_contract() {
    expected_binary=cup
    inspection_keys='format platform configuration inspection_policy binary sha256'

    case "$EXPECTED_PLATFORM" in
        linux-x64|linux-arm64)
            expected_format=ELF
            case "$EXPECTED_PLATFORM" in
                linux-x64) expected_arch=x86_64 ;;
                linux-arm64) expected_arch=aarch64 ;;
            esac
            inspection_keys="$inspection_keys object_format architecture elf_class elf_data"
            inspection_keys="$inspection_keys elf_type machine entry_point linkage interpreter"
            inspection_keys="$inspection_keys needed_count runtime_search_path file_description"
            ;;
        macos-x64|macos-arm64)
            expected_format=Mach-O
            case "$EXPECTED_PLATFORM" in
                macos-x64) expected_arch=x86_64 ;;
                macos-arm64) expected_arch=arm64 ;;
            esac
            inspection_keys="$inspection_keys object_format architecture minimum_os linkage"
            inspection_keys="$inspection_keys third_party_linkage system_linkage needed_count"
            inspection_keys="$inspection_keys runtime_search_path file_description"
            ;;
        windows-x64)
            expected_format='PE32+'
            expected_arch=x86_64
            expected_binary=cup.exe
            inspection_keys="$inspection_keys object_format architecture subsystem linkage"
            inspection_keys="$inspection_keys needed_count resource_directory dynamic_base"
            inspection_keys="$inspection_keys nx_compat runtime_search_path file_description"
            ;;
        *)
            fail "unsupported evidence platform: $EXPECTED_PLATFORM"
            ;;
    esac
}

verify_inspection_common() {
    actual_inspection_keys=$(sed '/^needed=/d; s/=.*//' "$INSPECTION" |
        tr '\n' ' ' | sed 's/ $//')
    [ "$actual_inspection_keys" = "$inspection_keys" ] ||
        fail 'binary-inspection.txt has an unexpected schema or key order'

    require_value inspection.format "$(exact_field "$INSPECTION" format)" 2
    require_value inspection.platform \
        "$(exact_field "$INSPECTION" platform)" "$EXPECTED_PLATFORM"
    require_value inspection.configuration \
        "$(exact_field "$INSPECTION" configuration)" development
    require_value inspection.policy \
        "$(exact_field "$INSPECTION" inspection_policy)" build
    require_value inspection.binary \
        "$(exact_field "$INSPECTION" binary)" "$expected_binary"
    require_value inspection.object_format \
        "$(exact_field "$INSPECTION" object_format)" "$expected_format"
    require_value inspection.architecture \
        "$(exact_field "$INSPECTION" architecture)" "$expected_arch"
    require_sha256 inspection.sha256 "$(exact_field "$INSPECTION" sha256)"

    needed_count=$(exact_field "$INSPECTION" needed_count)
    case "$needed_count" in
        ''|*[!0-9]*)
            fail 'inspection needed_count is not numeric'
            ;;
    esac
    actual_needed=$(awk -F= '$1 == "needed" { count++ } END { print count + 0 }' \
        "$INSPECTION")
    [ "$actual_needed" -eq "$needed_count" ] ||
        fail 'inspection needed_count does not match needed entries'
    [ "$needed_count" -gt 0 ] ||
        fail 'development inspection must contain dynamic system dependencies'
    awk -F= '$1 == "needed" && length($2) == 0 { invalid=1 }
        END { exit invalid ? 1 : 0 }' "$INSPECTION" ||
        fail 'inspection contains an empty needed entry'
    [ -n "$(exact_field "$INSPECTION" file_description)" ] ||
        fail 'inspection file_description is empty'
    require_value inspection.runtime_search_path \
        "$(exact_field "$INSPECTION" runtime_search_path)" none
}

verify_inspection_platform() {
    case "$EXPECTED_PLATFORM" in
        linux-x64|linux-arm64)
            require_value inspection.linkage \
                "$(exact_field "$INSPECTION" linkage)" dynamic-system
            interpreter=$(exact_field "$INSPECTION" interpreter)
            [ -n "$interpreter" ] && [ "$interpreter" != none ] ||
                fail 'Linux development inspection has no dynamic interpreter'
            ;;
        macos-x64|macos-arm64)
            require_value inspection.minimum_os \
                "$(exact_field "$INSPECTION" minimum_os)" 13.0
            require_value inspection.linkage \
                "$(exact_field "$INSPECTION" linkage)" \
                third-party-static-system-dynamic
            require_value inspection.third_party_linkage \
                "$(exact_field "$INSPECTION" third_party_linkage)" static
            require_value inspection.system_linkage \
                "$(exact_field "$INSPECTION" system_linkage)" dynamic
            ;;
        windows-x64)
            require_value inspection.subsystem \
                "$(exact_field "$INSPECTION" subsystem)" 'Windows CUI'
            require_value inspection.linkage \
                "$(exact_field "$INSPECTION" linkage)" dynamic-system
            require_value inspection.resource_directory \
                "$(exact_field "$INSPECTION" resource_directory)" present
            require_value inspection.dynamic_base \
                "$(exact_field "$INSPECTION" dynamic_base)" yes
            require_value inspection.nx_compat \
                "$(exact_field "$INSPECTION" nx_compat)" yes
            ;;
    esac
}

verify_candidate_config() {
    [ -n "$CANDIDATE_CONFIG" ] || return 0

    cup_path_require_regular_file "$CANDIDATE_CONFIG" 'candidate build config' ||
        fail "unsafe candidate build config: $CANDIDATE_CONFIG"
    require_canonical_text "$CANDIDATE_CONFIG"

    actual_candidate_keys=$(sed 's/=.*//' "$CANDIDATE_CONFIG")
    [ "$actual_candidate_keys" = "$BUILD_KEYS" ] ||
        fail 'candidate build config has an unexpected schema or key order'

    require_value candidate.format "$(exact_field "$CANDIDATE_CONFIG" format)" 3
    require_value candidate.platform \
        "$(exact_field "$CANDIDATE_CONFIG" platform)" "$EXPECTED_PLATFORM"
    require_value candidate.configuration \
        "$(exact_field "$CANDIDATE_CONFIG" configuration)" release
    require_value candidate.official_build \
        "$(exact_field "$CANDIDATE_CONFIG" official_build)" 1

    candidate_toolchain=$(exact_field "$CANDIDATE_CONFIG" dependency_toolchain_sha256)
    candidate_lock=$(exact_field "$CANDIDATE_CONFIG" dependency_source_lock_sha256)
    require_sha256 candidate.dependency_toolchain_sha256 "$candidate_toolchain"
    require_sha256 candidate.dependency_source_lock_sha256 "$candidate_lock"
    require_value candidate.dependency_platform \
        "$(exact_field "$CANDIDATE_CONFIG" dependency_platform)" "$EXPECTED_PLATFORM"
    require_value candidate.dependency_prefix_format \
        "$(exact_field "$CANDIDATE_CONFIG" dependency_prefix_format)" "$source_prefix_format"
    require_value candidate.dependency_profile \
        "$(exact_field "$CANDIDATE_CONFIG" dependency_profile)" "$source_profile"
    require_value candidate.dependency_build_revision \
        "$(exact_field "$CANDIDATE_CONFIG" dependency_build_revision)" "$source_revision"

    [ "$source_lock" = "$candidate_lock" ] ||
        fail 'release source lock differs from the tested source lock'
    [ "$source_toolchain" = "$candidate_toolchain" ] ||
        fail 'release dependency toolchain differs from the tested source dependency toolchain'

    candidate_prefix=$(exact_field "$CANDIDATE_CONFIG" deps_prefix)
    cup_path_validate_absolute_clean "$candidate_prefix" 'candidate dependency prefix' ||
        fail 'candidate dependency prefix is not an absolute clean path'

    candidate_compiler_command=$(exact_field "$CANDIDATE_CONFIG" compiler_command)
    candidate_compiler_path=$(exact_field "$CANDIDATE_CONFIG" compiler_path)
    candidate_compiler_target=$(exact_field "$CANDIDATE_CONFIG" compiler_target)
    candidate_compiler_target_normalized=$(exact_field "$CANDIDATE_CONFIG" compiler_target_normalized)
    candidate_compiler_version=$(exact_field "$CANDIDATE_CONFIG" compiler_version)
    candidate_compiler_numeric=$(exact_field "$CANDIDATE_CONFIG" compiler_numeric)
    require_nonempty candidate.compiler_command "$candidate_compiler_command"
    require_nonempty candidate.compiler_path "$candidate_compiler_path"
    require_nonempty candidate.compiler_target "$candidate_compiler_target"
    require_nonempty candidate.compiler_target_normalized "$candidate_compiler_target_normalized"
    require_nonempty candidate.compiler_version "$candidate_compiler_version"
    require_nonempty candidate.compiler_numeric "$candidate_compiler_numeric"
    cup_path_validate_absolute_clean "$candidate_compiler_path" 'candidate compiler path' ||
        fail 'candidate compiler path is not an absolute clean path'
    require_value candidate.compiler_command \
        "$candidate_compiler_command" "$source_compiler_command"
    require_value candidate.compiler_target_normalized \
        "$candidate_compiler_target_normalized" "$source_compiler_target_normalized"
    require_value candidate.compiler_numeric \
        "$candidate_compiler_numeric" "$source_compiler_numeric"

    candidate_windres_command=$(exact_field "$CANDIDATE_CONFIG" windres_command)
    candidate_windres_path=$(exact_field "$CANDIDATE_CONFIG" windres_path)
    candidate_windres_version=$(exact_field "$CANDIDATE_CONFIG" windres_version)
    candidate_windres_numeric=$(exact_field "$CANDIDATE_CONFIG" windres_numeric)
    candidate_windres_target_normalized=$(exact_field "$CANDIDATE_CONFIG" windres_target_normalized)
    if [ "$EXPECTED_PLATFORM" = windows-x64 ]; then
        require_nonempty candidate.windres_command "$candidate_windres_command"
        require_nonempty candidate.windres_path "$candidate_windres_path"
        require_nonempty candidate.windres_version "$candidate_windres_version"
        require_nonempty candidate.windres_numeric "$candidate_windres_numeric"
        require_nonempty candidate.windres_target_normalized "$candidate_windres_target_normalized"
        cup_path_validate_absolute_clean "$candidate_windres_path" 'candidate windres path' ||
            fail 'candidate windres path is not an absolute clean path'
        require_value candidate.windres_command \
            "$candidate_windres_command" "$source_windres_command"
        require_value candidate.windres_numeric \
            "$candidate_windres_numeric" "$source_windres_numeric"
        require_value candidate.windres_target_normalized \
            "$candidate_windres_target_normalized" "$source_windres_target_normalized"
    elif [ -n "$candidate_windres_command" ] || [ -n "$candidate_windres_path" ] ||
        [ -n "$candidate_windres_version" ] || [ -n "$candidate_windres_numeric" ] ||
        [ -n "$candidate_windres_target_normalized" ]; then
        fail 'non-Windows candidate build contains windres identity'
    fi
}

parse_arguments "$@"
resolve_and_validate_inputs
validate_evidence_directory
verify_envelope
load_source_build_contract
verify_release_metadata
select_inspection_contract
verify_inspection_common
verify_inspection_platform
verify_candidate_config

printf 'Source evidence verified: %s (%s at %s).\n' \
    "$EXPECTED_PLATFORM" "$EXPECTED_VERSION" "$EXPECTED_COMMIT"
