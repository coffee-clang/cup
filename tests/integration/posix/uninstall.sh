#!/bin/sh

# Purpose: Exercises the public detached uninstall workflow and canonical-root cleanup.
set -eu

TESTS_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
. "$TESTS_ROOT/support/posix/cli.sh"
. "$TESTS_ROOT/support/posix/uninstall.sh"

test_begin uninstall
prepare_command_environment
run_cup repair >/dev/null

cup_root=$TEST_HOME/.cup
printf 'fixture
' > "$cup_root/components/fixture.txt"
output=$(run_cup uninstall --yes)
assert_contains "$output" 'Uninstall started. The PATH entry was not removed.'

if ! cup_test_wait_for_uninstall "$cup_root" "$TEST_HOME"; then
    residue=$(cup_test_uninstall_residue "$TEST_HOME")
    assert_missing "$cup_root"
    fail "uninstall helper left staging behind: $residue"
fi


# A failed cleanup must keep enough ownership evidence for the installer to recognize and retry
# the detached residue. The mock fails only when the helper reaches the components directory.
prepare_command_environment
run_cup repair >/dev/null
cup_root=$TEST_HOME/.cup
mkdir -p "$cup_root/components" "$cup_root/bin"
printf 'fixture\n' > "$cup_root/components/fixture.txt"
cp "$CUP" "$cup_root/bin/cup"
chmod +x "$cup_root/bin/cup"

mock_bin=$TMP_ROOT/uninstall-mock-bin
mkdir -p "$mock_bin"
real_rm=$(command -v rm)
cat > "$mock_bin/rm" <<EOF_MOCK_RM
#!/bin/sh
for argument in "\$@"; do
    case "\$argument" in
        "\${CUP_TEST_FAIL_ROOT}"/.cup-uninstall.*/components)
            exit 1
            ;;
    esac
done
exec "$real_rm" "\$@"
EOF_MOCK_RM
chmod +x "$mock_bin/rm"

output=$(CUP_TEST_FAIL_ROOT="$TEST_HOME" PATH="$mock_bin:$PATH" run_cup uninstall --yes)
assert_contains "$output" 'Uninstall started. The PATH entry was not removed.'

residue=
attempt=0
while [ "$attempt" -lt 200 ]; do
    residue=$(cup_test_uninstall_residue "$TEST_HOME")
    if [ -n "$residue" ] && [ -f "$residue/transaction.txt" ] &&
        grep -F 'phase=failed' "$residue/transaction.txt" >/dev/null 2>&1 &&
        grep -F 'stage=cleanup' "$residue/transaction.txt" >/dev/null 2>&1; then
        break
    fi
    sleep 0.1
    attempt=$((attempt + 1))
done
[ -n "$residue" ] || fail 'failed uninstall did not leave a detached residue'
assert_missing "$cup_root"
assert_file "$residue/root.txt"
assert_file "$residue/bin/cup"
assert_file "$residue/transaction.txt"
assert_file "$residue/components/fixture.txt"

printf 'Uninstall integration tests passed for %s.
' "$TEST_PLATFORM"
