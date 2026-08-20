#!/bin/sh

# Exercises the public detached uninstall workflow and canonical-root cleanup.
set -eu

TESTS_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
. "$TESTS_ROOT/support/posix/cli.sh"
. "$TESTS_ROOT/support/posix/uninstall.sh"

test_begin uninstall
prepare_command_environment
run_cup repair >/dev/null

# The detached helper independently parses transaction.txt. Keep its token grammar identical to
# the C journal parser: 256 bytes is already outside MAX_TRANSACTION_TOKEN_LEN.
parser_home=$TMP_ROOT/uninstall-parser-home
parser_root=$parser_home/.cup
parser_script=$TMP_ROOT/uninstall-parser.sh
mkdir -p "$parser_root"
cp "$PROJECT_ROOT/scripts/install/uninstall-cup.sh" "$parser_script"
chmod +x "$parser_script"
printf 'format=1\nproduct=coffee-clang/cup\nlayout=1\n' > "$parser_root/root.txt"
long_token=$(printf '%0256d' 0)
printf 'format=1\noperation=uninstall\nphase=scheduled\ntemporary_name=.cup-uninstall-%s\ntoken=%s\nstage=handoff\nerror=0\n' \
    "$long_token" "$long_token" > "$parser_root/transaction.txt"
if parser_output=$(HOME="$parser_home" "$parser_script" "$parser_root" "$parser_script" 3 3</dev/null 2>&1); then
    fail 'POSIX uninstall helper accepted an oversized transaction token'
fi
assert_contains "$parser_output" 'uninstall token is invalid'

cup_root=$TEST_HOME/.cup
printf 'fixture\n' > "$cup_root/components/fixture.txt"
output=$(run_cup uninstall --yes)
assert_contains "$output" 'Uninstall started. The PATH entry was not removed.'

if ! cup_test_wait_for_uninstall "$cup_root" "$TEST_HOME"; then
    residue=$(cup_test_uninstall_residue "$TEST_HOME")
    assert_missing "$cup_root"
    fail "uninstall helper left staging behind: $residue"
fi


# A failed cleanup must keep enough ownership evidence to identify the detached residue without
# guessing at unrelated data. The mock fails only when the helper reaches the components directory.
prepare_command_environment
run_cup repair >/dev/null
cup_root=$TEST_HOME/.cup
mkdir -p "$cup_root/components" "$cup_root/bin"
printf 'fixture\n' > "$cup_root/components/fixture.txt"
cp "$CUP" "$cup_root/bin/cup"
chmod +x "$cup_root/bin/cup"

mock_bin=$TMP_ROOT/uninstall-mock-bin
mkdir -p "$mock_bin"
real_find=$(command -v find)
cat > "$mock_bin/find" <<EOF_MOCK_FIND
#!/bin/sh
target=
for argument do
    case "\$argument" in
        -*) ;;
        *) target=\$argument; break ;;
    esac
done
case "\$target" in
    "\${CUP_TEST_FAIL_ROOT}"/.cup-uninstall.*/components|\
    "\${CUP_TEST_FAIL_ROOT}"/.cup-uninstall-*/components)
        exit 1
        ;;
esac
exec "$real_find" "\$@"
EOF_MOCK_FIND
chmod +x "$mock_bin/find"

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

printf 'Uninstall integration tests passed for %s.\n' "$TEST_PLATFORM"
