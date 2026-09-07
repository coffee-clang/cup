#!/bin/sh

# Verifies that a release build uses the toolchain/dependency identity exercised by source tests.
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
PROJECT_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/../.." && pwd -P)
# shellcheck source=../lib/text-file.sh
. "$PROJECT_ROOT/scripts/lib/text-file.sh"

[ "$#" -eq 3 ] || {
    printf 'Usage: %s <source-build-config> <candidate-build-config> <platform>\n' "$0" >&2
    exit 2
}
SOURCE_CONFIG=$1
CANDIDATE_CONFIG=$2
PLATFORM=$3

fail() {
    printf 'source build config: %s\n' "$*" >&2
    exit 1
}

case "$PLATFORM" in
    linux-x64|linux-arm64|macos-x64|macos-arm64|windows-x64) ;;
    *) fail "unsupported platform: $PLATFORM" ;;
esac

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

field() {
    field_file=$1
    field_key=$2
    sed -n "s/^${field_key}=//p" "$field_file"
}

require_file() {
    file_path=$1
    [ -f "$file_path" ] && [ ! -L "$file_path" ] && [ -s "$file_path" ] ||
        fail "missing or unsafe build config: $file_path"
    cup_text_file_is_nul_cr_free "$file_path" ||
        fail "build config contains a NUL or carriage-return byte: $file_path"
    actual_keys=$(sed 's/=.*//' "$file_path")
    [ "$actual_keys" = "$BUILD_KEYS" ] ||
        fail "build config has an unexpected schema or key order: $file_path"
}

require_value() {
    value_label=$1
    value_actual=$2
    value_expected=$3
    [ "$value_actual" = "$value_expected" ] ||
        fail "$value_label '$value_actual' does not match '$value_expected'"
}

require_nonempty() {
    value_label=$1
    value_actual=$2
    [ -n "$value_actual" ] && [ "$value_actual" != missing ] ||
        fail "$value_label is empty or unavailable"
}

require_sha256() {
    value_label=$1
    value_actual=$2
    printf '%s\n' "$value_actual" | grep -Eq '^[0-9a-f]{64}$' ||
        fail "$value_label is not a lowercase SHA-256 value"
}

validate_role() {
    role_file=$1
    role_name=$2
    role_configuration=$3
    role_official=$4

    require_value "$role_name.format" "$(field "$role_file" format)" 3
    require_value "$role_name.platform" "$(field "$role_file" platform)" "$PLATFORM"
    require_value "$role_name.configuration" \
        "$(field "$role_file" configuration)" "$role_configuration"
    require_value "$role_name.official_build" \
        "$(field "$role_file" official_build)" "$role_official"
    require_value "$role_name.dependency_platform" \
        "$(field "$role_file" dependency_platform)" "$PLATFORM"
    require_value "$role_name.compiler_target_normalized" \
        "$(field "$role_file" compiler_target_normalized)" "$PLATFORM"

    require_nonempty "$role_name.compiler_command" "$(field "$role_file" compiler_command)"
    require_nonempty "$role_name.compiler_numeric" "$(field "$role_file" compiler_numeric)"
    require_nonempty "$role_name.dependency_prefix_format" \
        "$(field "$role_file" dependency_prefix_format)"
    require_nonempty "$role_name.dependency_profile" "$(field "$role_file" dependency_profile)"
    require_nonempty "$role_name.dependency_build_revision" \
        "$(field "$role_file" dependency_build_revision)"
    require_sha256 "$role_name.dependency_source_lock_sha256" \
        "$(field "$role_file" dependency_source_lock_sha256)"
    require_sha256 "$role_name.dependency_toolchain_sha256" \
        "$(field "$role_file" dependency_toolchain_sha256)"

    if [ "$PLATFORM" = windows-x64 ]; then
        require_nonempty "$role_name.windres_command" "$(field "$role_file" windres_command)"
        require_nonempty "$role_name.windres_numeric" "$(field "$role_file" windres_numeric)"
        require_value "$role_name.windres_target_normalized" \
            "$(field "$role_file" windres_target_normalized)" windows-x64
    else
        for windres_key in windres_command windres_path windres_version windres_numeric \
                windres_target_normalized; do
            [ -z "$(field "$role_file" "$windres_key")" ] ||
                fail "$role_name contains unexpected $windres_key identity"
        done
    fi
}

require_file "$SOURCE_CONFIG"
require_file "$CANDIDATE_CONFIG"
validate_role "$SOURCE_CONFIG" source development 0
validate_role "$CANDIDATE_CONFIG" candidate release 1

for key in dependency_prefix_format dependency_profile dependency_build_revision \
        dependency_source_lock_sha256 dependency_toolchain_sha256 \
        compiler_command compiler_target_normalized compiler_numeric; do
    source_value=$(field "$SOURCE_CONFIG" "$key")
    candidate_value=$(field "$CANDIDATE_CONFIG" "$key")
    [ "$candidate_value" = "$source_value" ] ||
        fail "candidate $key '$candidate_value' differs from source-tested '$source_value'"
done

if [ "$PLATFORM" = windows-x64 ]; then
    for key in windres_command windres_numeric windres_target_normalized; do
        source_value=$(field "$SOURCE_CONFIG" "$key")
        candidate_value=$(field "$CANDIDATE_CONFIG" "$key")
        [ "$candidate_value" = "$source_value" ] ||
            fail "candidate $key '$candidate_value' differs from source-tested '$source_value'"
    done
fi

printf 'Source build config verified: %s.\n' "$PLATFORM"
