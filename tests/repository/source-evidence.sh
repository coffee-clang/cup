#!/bin/sh

# Verifies exact source-test evidence schemas and release linkage.
set -eu

TESTS_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
. "$TESTS_ROOT/support/common.sh"

test_begin source-evidence

VERIFIER=$PROJECT_ROOT/scripts/ci/verify-source-evidence.sh
EVIDENCE=$TMP_ROOT/evidence
CANDIDATE=$TMP_ROOT/release-build-config.txt
VERSION=$(cat "$PROJECT_ROOT/VERSION")
COMMIT=0123456789abcdef0123456789abcdef01234567
REPOSITORY=example/cup
RUN_ID=31
RUN_ATTEMPT=2
ARTIFACT=cup-source-evidence-linux-x64-attempt-2
LOCK_SHA=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa
TOOLCHAIN_SHA=bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb
BINARY_SHA=cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc
mkdir -p "$EVIDENCE"

write_build_config() {
    destination=$1
    configuration=$2
    official=$3
    toolchain=$4
    cat >"$destination" <<EOF_CONFIG
format=3
platform=linux-x64
configuration=$configuration
host_system=Linux
host_machine=x86_64
compiler_command=gcc
compiler_path=/usr/bin/gcc
compiler_target=x86_64-linux-gnu
compiler_target_normalized=linux-x64
compiler_version=gcc fixture
compiler_numeric=14.0.0
windres_command=
windres_path=
windres_version=
windres_numeric=
windres_target_normalized=
cppflags=-D_POSIX_C_SOURCE=200809L
cflags=-std=c11
ldflags=
ldlibs=-lc
deps_prefix=/tmp/cup-source-evidence-prefix
dependency_prefix_format=5
dependency_platform=linux-x64
dependency_profile=gcc
dependency_build_revision=4
dependency_source_lock_sha256=$LOCK_SHA
dependency_toolchain_sha256=$toolchain
official_build=$official
EOF_CONFIG
}

write_fixture() {
    rm -rf "$EVIDENCE"
    mkdir -p "$EVIDENCE"
    write_build_config "$EVIDENCE/build-config.txt" development 0 "$TOOLCHAIN_SHA"
    cat >"$EVIDENCE/release.txt" <<EOF_RELEASE
format=1
version=$VERSION
commit=$COMMIT
EOF_RELEASE
    cat >"$EVIDENCE/binary-inspection.txt" <<EOF_INSPECTION
format=2
platform=linux-x64
configuration=development
inspection_policy=build
binary=cup
sha256=$BINARY_SHA
object_format=ELF
architecture=x86_64
elf_class=ELF64
elf_data=2's complement, little endian
elf_type=DYN
machine=Advanced Micro Devices X86-64
entry_point=0x1000
linkage=dynamic-system
interpreter=/lib64/ld-linux-x86-64.so.2
needed_count=1
needed=libc.so.6
runtime_search_path=none
file_description=ELF 64-bit LSB pie executable
EOF_INSPECTION
    cat >"$EVIDENCE/evidence.txt" <<EOF_EVIDENCE
format=1
version=$VERSION
source_repository=$REPOSITORY
source_commit=$COMMIT
run_id=$RUN_ID
run_attempt=$RUN_ATTEMPT
artifact_name=$ARTIFACT
platform=linux-x64
build_config_sha256=$(sha256sum "$EVIDENCE/build-config.txt" | awk '{print $1}')
release_sha256=$(sha256sum "$EVIDENCE/release.txt" | awk '{print $1}')
binary_inspection_sha256=$(sha256sum "$EVIDENCE/binary-inspection.txt" | awk '{print $1}')
EOF_EVIDENCE
    write_build_config "$CANDIDATE" release 1 "$TOOLCHAIN_SHA"
}

refresh_envelope() {
    cat >"$EVIDENCE/evidence.txt" <<EOF_EVIDENCE
format=1
version=$VERSION
source_repository=$REPOSITORY
source_commit=$COMMIT
run_id=$RUN_ID
run_attempt=$RUN_ATTEMPT
artifact_name=$ARTIFACT
platform=linux-x64
build_config_sha256=$(sha256sum "$EVIDENCE/build-config.txt" | awk '{print $1}')
release_sha256=$(sha256sum "$EVIDENCE/release.txt" | awk '{print $1}')
binary_inspection_sha256=$(sha256sum "$EVIDENCE/binary-inspection.txt" | awk '{print $1}')
EOF_EVIDENCE
}

verify_ok() {
    "$VERIFIER" "$EVIDENCE" linux-x64 "$REPOSITORY" "$COMMIT" \
        "$RUN_ID" "$RUN_ATTEMPT" "$ARTIFACT" "$CANDIDATE" >/dev/null
}

expect_failure() {
    label=$1
    shift
    if "$VERIFIER" "$EVIDENCE" linux-x64 "$REPOSITORY" "$COMMIT" \
            "$RUN_ID" "$RUN_ATTEMPT" "$ARTIFACT" "$CANDIDATE" \
            >"$TMP_ROOT/$label.out" 2>&1; then
        fail "$label source evidence was accepted"
    fi
    "$@"
}

write_fixture
verify_ok

rm "$EVIDENCE/release.txt"
expect_failure missing-release grep -Fq 'expected files' "$TMP_ROOT/missing-release.out"

write_fixture
printf 'extra\n' >"$EVIDENCE/extra.txt"
expect_failure extra-file grep -Fq 'expected files' "$TMP_ROOT/extra-file.out"

write_fixture
printf '\r' >>"$EVIDENCE/build-config.txt"
expect_failure carriage-return grep -Eq 'not LF-terminated|non-canonical bytes' \
    "$TMP_ROOT/carriage-return.out"


write_fixture
printf '\0\n' >>"$EVIDENCE/release.txt"
expect_failure nul-byte grep -Fq 'non-canonical bytes' "$TMP_ROOT/nul-byte.out"

write_fixture
printf 'format=1\nversion=%s\ncommit=%s' "$VERSION" "$COMMIT" \
    >"$EVIDENCE/release.txt"
expect_failure missing-final-lf grep -Fq 'not LF-terminated' \
    "$TMP_ROOT/missing-final-lf.out"

write_fixture
printf 'platform=linux-x64\n' >>"$EVIDENCE/build-config.txt"
refresh_envelope
expect_failure duplicate-field grep -Eq 'unexpected schema|exactly one' "$TMP_ROOT/duplicate-field.out"

write_fixture
sed -i 's/^platform=linux-x64$/platform=linux-arm64/' "$EVIDENCE/binary-inspection.txt"
refresh_envelope
expect_failure wrong-platform grep -Fq "does not match 'linux-x64'" "$TMP_ROOT/wrong-platform.out"

write_fixture
sed -i 's/^needed_count=1$/needed_count=2/' "$EVIDENCE/binary-inspection.txt"
refresh_envelope
expect_failure needed-count grep -Fq 'needed_count does not match' "$TMP_ROOT/needed-count.out"

write_fixture
sed -i 's/^linkage=dynamic-system$/linkage=static/' "$EVIDENCE/binary-inspection.txt"
refresh_envelope
expect_failure wrong-linkage grep -Fq "does not match 'dynamic-system'" "$TMP_ROOT/wrong-linkage.out"

write_fixture
sed -i 's/^runtime_search_path=none$/runtime_search_path=\/tmp\/foreign/' \
    "$EVIDENCE/binary-inspection.txt"
refresh_envelope
expect_failure runtime-search-path grep -Fq "does not match 'none'" \
    "$TMP_ROOT/runtime-search-path.out"

write_fixture
sed -i 's/^compiler_command=gcc$/compiler_command=/' "$EVIDENCE/build-config.txt"
refresh_envelope
expect_failure empty-source-compiler grep -Fq 'compiler_command is empty or unavailable' \
    "$TMP_ROOT/empty-source-compiler.out"

write_fixture
sed -i 's/^compiler_command=gcc$/compiler_command=clang/' "$CANDIDATE"
expect_failure compiler-command-mismatch grep -Fq 'candidate.compiler_command' \
    "$TMP_ROOT/compiler-command-mismatch.out"

write_fixture
sed -i 's/^compiler_target_normalized=linux-x64$/compiler_target_normalized=linux-arm64/' \
    "$CANDIDATE"
expect_failure compiler-target-mismatch grep -Fq 'candidate.compiler_target_normalized' \
    "$TMP_ROOT/compiler-target-mismatch.out"

write_fixture
sed -i 's/^compiler_numeric=14.0.0$/compiler_numeric=15.0.0/' "$CANDIDATE"
expect_failure compiler-version-mismatch grep -Fq 'candidate.compiler_numeric' \
    "$TMP_ROOT/compiler-version-mismatch.out"

write_fixture
sed -i 's|^compiler_path=/usr/bin/gcc$|compiler_path=/opt/toolchain/bin/gcc|' "$CANDIDATE"
sed -i 's/^compiler_target=x86_64-linux-gnu$/compiler_target=x86_64-unknown-linux-gnu/' \
    "$CANDIDATE"
sed -i 's/^compiler_version=gcc fixture$/compiler_version=gcc fixture from release runner/' \
    "$CANDIDATE"
verify_ok

write_fixture
write_build_config "$CANDIDATE" release 1 \
    dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd
expect_failure toolchain-mismatch grep -Fq 'dependency toolchain differs' \
    "$TMP_ROOT/toolchain-mismatch.out"

write_fixture
printf 'unexpected=value\n' >>"$CANDIDATE"
expect_failure candidate-schema \
    grep -Fq 'candidate build config has an unexpected schema' \
    "$TMP_ROOT/candidate-schema.out"

write_fixture
awk -v replacement=dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd '
    /^dependency_source_lock_sha256=/ { print "dependency_source_lock_sha256=" replacement; next }
    { print }
' "$CANDIDATE" >"$TMP_ROOT/candidate-lock.txt"
mv "$TMP_ROOT/candidate-lock.txt" "$CANDIDATE"
expect_failure source-lock-mismatch grep -Fq 'source lock differs' \
    "$TMP_ROOT/source-lock-mismatch.out"

write_fixture
sed -i 's/^run_attempt=2$/run_attempt=3/' "$EVIDENCE/evidence.txt"
expect_failure wrong-attempt grep -Fq 'envelope or artifact identity' "$TMP_ROOT/wrong-attempt.out"

write_fixture
sed -i 's/^artifact_name=.*/artifact_name=cup-source-evidence-linux-x64-attempt-3/' \
    "$EVIDENCE/evidence.txt"
expect_failure wrong-artifact grep -Fq 'envelope or artifact identity' "$TMP_ROOT/wrong-artifact.out"

write_fixture
wrong_digest=dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd
sed -i "s/^build_config_sha256=.*/build_config_sha256=$wrong_digest/" \
    "$EVIDENCE/evidence.txt"
expect_failure wrong-digest grep -Fq 'envelope or artifact identity' "$TMP_ROOT/wrong-digest.out"


SOURCE_WRITER=$PROJECT_ROOT/scripts/ci/write-source-evidence.sh
write_fixture
ACTUAL_COMMIT=$COMMIT
WRITTEN=$TMP_ROOT/written-source-evidence
"$SOURCE_WRITER" "$WRITTEN" linux-x64 "$EVIDENCE/build-config.txt" \
    "$EVIDENCE/release.txt" "$EVIDENCE/binary-inspection.txt" \
    "$REPOSITORY" "$RUN_ID" "$RUN_ATTEMPT" >/dev/null
written_before=$(sha256sum "$WRITTEN/evidence.txt" | awk '{print $1}')
if "$SOURCE_WRITER" "$WRITTEN" linux-x64 "$EVIDENCE/build-config.txt" \
        "$EVIDENCE/release.txt" "$EVIDENCE/binary-inspection.txt" \
        "$REPOSITORY" "$RUN_ID" "$RUN_ATTEMPT" \
        >"$TMP_ROOT/written-existing.out" 2>&1; then
    fail 'source evidence writer replaced an existing destination'
fi
[ "$(sha256sum "$WRITTEN/evidence.txt" | awk '{print $1}')" = "$written_before" ] ||
    fail 'failed source evidence rewrite changed committed evidence'
grep -Fq 'already exists' "$TMP_ROOT/written-existing.out"

PATH=$TMP_ROOT/shasum-only:$PATH
mkdir -p "$TMP_ROOT/shasum-only"
cat > "$TMP_ROOT/shasum-only/shasum" <<'EOF_SHASUM'
#!/bin/sh
[ "$1" = -a ] && [ "$2" = 256 ] || exit 2
shift 2
exec /usr/bin/sha256sum "$@"
EOF_SHASUM
chmod 0755 "$TMP_ROOT/shasum-only/shasum"
for command in sha256sum; do
    cat > "$TMP_ROOT/shasum-only/$command" <<'EOF_DISABLED'
#!/bin/sh
exit 127
EOF_DISABLED
    chmod 0755 "$TMP_ROOT/shasum-only/$command"
done
# A shell with only the shasum-compatible path must still validate the envelope.
PATH=$TMP_ROOT/shasum-only:/usr/bin:/bin \
    "$VERIFIER" "$WRITTEN" linux-x64 "$REPOSITORY" \
    "$ACTUAL_COMMIT" "$RUN_ID" "$RUN_ATTEMPT" \
    "$ARTIFACT" >/dev/null

printf 'Source-evidence writer and verifier contract tests passed.\n'
