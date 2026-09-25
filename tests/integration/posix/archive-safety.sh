#!/bin/sh

# Exercises supported formats and malicious archive rejection through the real POSIX CLI.
set -eu

TESTS_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
. "$TESTS_ROOT/support/posix/cli.sh"

test_begin archive-safety
prepare_command_environment
absolute_escape=$TMP_ROOT/absolute-escape.txt
export CUP_TEST_ABSOLUTE_ESCAPE_PATH=$absolute_escape

install_supported_format() {
    version=$1
    format=$2
    make_package_format compiler clang "$version" "$TEST_PLATFORM" "$format" clang
    if [ "$format" = tar.gz ]; then
        run_cup install compiler "clang@$version" >/dev/null
    else
        run_cup install compiler "clang@$version" --format "$format" >/dev/null
    fi
    assert_contains "$(run_cup list compiler)" "compiler:clang@$version"
    assert_cup_healthy
}

install_supported_format 98.0.1 tar.gz
install_supported_format 98.0.2 tar.xz
install_supported_format 98.0.3 zip

# Malformed cached archives remain local fixture failures; no public endpoint is contacted.

create_mismatched_archive() {
    version=$1
    declared_format=$2
    actual_format=$3
    make_package_format compiler clang "$version" "$TEST_PLATFORM" "$actual_format" clang
    package_name=clang-$version-$TEST_PLATFORM-$TEST_PLATFORM
    artifact=$TMP_ROOT/artifacts/$package_name.$actual_format
    sha=$(hash_file "$artifact")
    package_catalog_rewrite_artifact clang "$version" "$declared_format" \
        "https://example.invalid/$package_name.$declared_format" "$sha"
}

create_plain_tar_disguised_as_gzip() {
    version=$1
    make_package_format compiler clang "$version" "$TEST_PLATFORM" tar.gz clang
    package_name=clang-$version-$TEST_PLATFORM-$TEST_PLATFORM
    package_root=$TMP_ROOT/packages/$package_name
    archive=$TMP_ROOT/artifacts/$package_name.tar.gz
    old_sha=$(awk -v i="$(package_catalog_find_index clang "$version")" -F= \
        '$1 == "package." i ".artifact.0.sha256" {print $2}' "$DEV_ROOT/config/catalog.cfg")
    rm -f "$archive" "$TEST_HOME/.cup/cache/$old_sha"
    tar -cf "$archive" -C "$TMP_ROOT/packages" "$package_name"
    sha=$(package_cache_publish "$archive")
    package_catalog_rewrite_artifact clang "$version" tar.gz \
        "https://example.invalid/$package_name.tar.gz" "$sha"
}

create_mismatched_archive 98.1.1 tar.xz tar.gz
run_cup_expect_failure "$TMP_ROOT/archive-format-mismatch.out" \
    install compiler clang@98.1.1 --format tar.xz
assert_contains "$(cat "$TMP_ROOT/archive-format-mismatch.out")" \
    "archive content does not match declared format 'tar.xz'"
assert_not_contains "$(run_cup list compiler 2>/dev/null)" 'compiler:clang@98.1.1'
assert_missing "$TEST_HOME/.cup/transaction.txt"
assert_cup_healthy

create_plain_tar_disguised_as_gzip 98.1.2
run_cup_expect_failure "$TMP_ROOT/archive-plain-tar.out" \
    install compiler clang@98.1.2
assert_contains "$(cat "$TMP_ROOT/archive-plain-tar.out")" \
    "archive content does not match declared format 'tar.gz'"
assert_not_contains "$(run_cup list compiler 2>/dev/null)" 'compiler:clang@98.1.2'
assert_missing "$TEST_HOME/.cup/transaction.txt"
assert_cup_healthy

create_unsafe_archive() {
    version=$1
    mode=$2
    component=compiler
    tool=clang
    target=$TEST_PLATFORM
    host=$TEST_PLATFORM
    package_name=$tool-$version-$host-$target
    artifact_dir=$TMP_ROOT/artifacts
    archive=$artifact_dir/$package_name.tar.gz
    mkdir -p "$artifact_dir"

    configuration=${CUP_TEST_CONFIGURATION:-development}
    test_build_root=${CUP_TEST_BUILD_ROOT:-$PROJECT_ROOT/build}
    fixture=$test_build_root/$TEST_PLATFORM/$configuration/tests/helpers/archive-fixture
    assert_file "$fixture"
    "$fixture" "$package_name" "$version" "$host" "$target" "$archive" "$mode"
    sha=$(package_cache_publish "$archive")
    package_catalog_add_package "$component" "$tool" "$target" "$version" tar.gz \
        "https://example.invalid/$package_name.tar.gz" "$sha"
}

# A producer-style POSIX command alias may be a confined relative symlink to a declared file.
safe_symlink_version=98.2.1
create_unsafe_archive "$safe_symlink_version" safe-symlink
run_cup install compiler "clang@$safe_symlink_version" >/dev/null
assert_contains "$(run_cup list compiler)" "compiler:clang@$safe_symlink_version"
assert_cup_healthy

index=1
for case in traversal absolute symlink symlink-parent duplicate case-collision \
        file-directory reserved unicode special hardlink-forward root-file; do
    version="99.0.$index"
    index=$((index + 1))
    create_unsafe_archive "$version" "$case"
    run_cup_expect_failure "$TMP_ROOT/archive-$case.out" \
        install compiler "clang@$version"
    output=$(cat "$TMP_ROOT/archive-$case.out")
    case "$case" in
        traversal|reserved)
            expected='archive contains an unsafe path'
            ;;
        unicode)
            expected=
            ;;
        absolute)
            expected='archive contains multiple or unsafe top-level roots'
            ;;
        symlink)
            expected='archive symbolic link escapes the package root'
            ;;
        symlink-parent|duplicate|case-collision|file-directory)
            expected='archive contains a duplicate, case-colliding, or path-type-colliding path'
            ;;
        special|hardlink-forward)
            expected='archive contains unsupported entry type'
            ;;
        root-file)
            expected='archive top-level root is not a directory'
            ;;
    esac
    [ -z "$expected" ] || assert_contains "$output" "$expected"
    assert_not_contains "$output" '==> Validating package...'
    assert_not_contains "$(run_cup list compiler 2>/dev/null)" "compiler:clang@$version"
    assert_missing "$TMP_ROOT/outside.txt"
    assert_missing "$absolute_escape"
    assert_missing "$TEST_HOME/.cup/transaction.txt"
done

assert_cup_healthy
printf 'Archive format and safety integration tests passed for %s.\n' "$TEST_PLATFORM"
