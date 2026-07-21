#!/bin/sh

# Purpose: Verifies that generated installer and release metadata agree with
# the selected official version.
set -eu

TESTS_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
. "$TESTS_ROOT/support/common.sh"

test_begin release-metadata
functions=$TMP_ROOT/install-functions.sh
sed '$d' "$PROJECT_ROOT/scripts/install/install-cup.sh" > "$functions"

validate() {
    file=$1
    sh -c '. "$1"; validate_release_metadata "$2"' sh "$functions" "$file"
}

expect_invalid() {
    file=$1
    if validate "$file" >/dev/null 2>&1; then
        fail "invalid release metadata unexpectedly succeeded: $file"
    fi
}

# Shell quoting and installer transport policy.
quoted_path=$(sh -c '. "$1"; shell_quote "$2"' sh \
    "$functions" "/tmp/cup path/with'a quote")
[ "$quoted_path" = "'/tmp/cup path/with'\\''a quote'" ] ||
    fail "shell_quote produced an unsafe result: $quoted_path"
resolved_path=$(sh -c '
    . "$1"
    quoted=$(shell_quote "$2")
    eval "value=$quoted"
    printf "%s" "$value"
' sh "$functions" "/tmp/cup path/with'a quote")
[ "$resolved_path" = "/tmp/cup path/with'a quote" ] ||
    fail 'shell_quote did not round-trip the path'


CUP_INSTALL_BASE_URL=https://example.invalid/releases sh -c '
    . "$1"
    validate_base_url
' sh "$functions"
if CUP_INSTALL_BASE_URL=http://example.invalid/releases sh -c \
        '. "$1"; validate_base_url' sh "$functions" >/dev/null 2>&1; then
    fail 'external HTTP installer URL unexpectedly succeeded'
fi
if CUP_INSTALL_BASE_URL=http://127.0.0.1:8080 sh -c \
        '. "$1"; validate_base_url' sh "$functions" >/dev/null 2>&1; then
    fail 'loopback HTTP installer URL without test opt-in unexpectedly succeeded'
fi
CUP_INSTALL_BASE_URL=http://127.0.0.1:8080 CUP_INSTALL_ALLOW_INSECURE=1 sh -c '
    . "$1"
    validate_base_url
' sh "$functions"
if CUP_INSTALL_BASE_URL=https://user@example.invalid/releases sh -c \
        '. "$1"; validate_base_url' sh "$functions" >/dev/null 2>&1; then
    fail 'installer URL containing credentials unexpectedly succeeded'
fi

# Common checksum files must contain one exact, safe asset set.
checksum_dir=$TMP_ROOT/checksums
mkdir -p "$checksum_dir"
printf '%s\n' packages > "$checksum_dir/packages.cfg"
printf '%s\n' policy > "$checksum_dir/install.cfg"
printf '%s\n' shell > "$checksum_dir/install.sh"
printf '%s\n' powershell > "$checksum_dir/install.ps1"
{
    for name in packages.cfg install.cfg install.sh install.ps1; do
        printf '%s  %s\n' "$(hash_file "$checksum_dir/$name")" "$name"
    done
} > "$checksum_dir/SHA256SUMS.common"
sh -c '
    . "$1"
    assert_checksum_entries "$2/SHA256SUMS.common" \
        packages.cfg install.cfg install.sh install.ps1
    verify_named_checksum "$2" "$2/SHA256SUMS.common" install.ps1
' sh "$functions" "$checksum_dir"

# Generated release metadata must be strict, complete and version-consistent.
cat > "$TMP_ROOT/valid" <<'EOF'
format=1
version=0.2.0
commit=abcdef0
EOF
validate "$TMP_ROOT/valid"

cat > "$TMP_ROOT/leading-zero" <<'EOF'
format=1
version=00.2.0
commit=abcdef0
EOF
expect_invalid "$TMP_ROOT/leading-zero"

cat > "$TMP_ROOT/non-ascii-digit" <<'EOF'
format=1
version=٠.2.0
commit=abcdef0
EOF
expect_invalid "$TMP_ROOT/non-ascii-digit"

cat > "$TMP_ROOT/too-large" <<'EOF'
format=1
version=1000000.2.0
commit=abcdef0
EOF
expect_invalid "$TMP_ROOT/too-large"

printf '%s\n' '0.2.0' > "$TMP_ROOT/plain"
expect_invalid "$TMP_ROOT/plain"

cat > "$TMP_ROOT/duplicate" <<'EOF'
format=1
version=0.2.0
version=0.2.1
commit=abcdef0
EOF
expect_invalid "$TMP_ROOT/duplicate"

cat > "$TMP_ROOT/extra" <<'EOF'
format=1
version=0.2.0
commit=abcdef0
unknown=value
EOF
expect_invalid "$TMP_ROOT/extra"

cat > "$TMP_ROOT/bad-commit" <<'EOF'
format=1
version=0.2.0
commit=not-a-commit
EOF
expect_invalid "$TMP_ROOT/bad-commit"

# Shell-profile integration must preserve existing content and reject link traversal.
fish_home=$TMP_ROOT/fish-home
mkdir -p "$fish_home"
HOME="$fish_home" SHELL=/usr/bin/fish PATH=/usr/bin:/bin sh -c '
    . "$1"
    CUP_BIN_DIR="$HOME/.cup/bin"
    prompt_tty() {
        printf "%s\n" y
    }
    offer_path_update >/dev/null
' sh "$functions"
fish_config=$fish_home/.config/fish/conf.d/cup.fish
[ -f "$fish_config" ] || fail 'Fish PATH configuration was not created'
[ "$(cat "$fish_config")" = 'fish_add_path "$HOME/.cup/bin"' ] ||
    fail 'Fish PATH configuration has unexpected contents'
HOME="$fish_home" SHELL=/usr/bin/fish PATH=/usr/bin:/bin sh -c '
    . "$1"
    CUP_BIN_DIR="$HOME/.cup/bin"
    prompt_tty() {
        printf "%s\n" y
    }
    offer_path_update >/dev/null
' sh "$functions"
[ "$(grep -Fxc 'fish_add_path "$HOME/.cup/bin"' "$fish_config")" = 1 ] ||
    fail 'Fish PATH configuration was duplicated'

preserved_home=$TMP_ROOT/fish-preserved-home
preserved_config=$preserved_home/.config/fish/conf.d/cup.fish
mkdir -p "${preserved_config%/*}"
printf '%s\n' '# existing user content' > "$preserved_config"
HOME="$preserved_home" SHELL=/usr/bin/fish PATH=/usr/bin:/bin sh -c '
    . "$1"
    CUP_BIN_DIR="$HOME/.cup/bin"
    prompt_tty() {
        printf "%s\n" y
    }
    offer_path_update >/dev/null
' sh "$functions"
grep -F '# existing user content' "$preserved_config" >/dev/null ||
    fail 'Fish PATH configuration overwrote existing content'
[ "$(grep -Fxc 'fish_add_path "$HOME/.cup/bin"' "$preserved_config")" = 1 ] ||
    fail 'Fish PATH configuration was not appended exactly once'


symlink_home=$TMP_ROOT/symlink-profile-home
external_profile=$TMP_ROOT/external-profile
mkdir -p "$symlink_home"
printf '%s\n' '# external content' > "$external_profile"
ln -s "$external_profile" "$symlink_home/.bashrc"
if HOME="$symlink_home" SHELL=/bin/bash PATH=/usr/bin:/bin sh -c '
    . "$1"
    CUP_BIN_DIR="$HOME/.cup/bin"
    prompt_tty() {
        printf "%s\n" y
    }
    offer_path_update
' sh "$functions" >"$TMP_ROOT/symlink-profile.out" 2>&1; then
    fail 'symbolic-link shell profile unexpectedly succeeded'
fi
[ "$(cat "$external_profile")" = '# external content' ] ||
    fail 'symbolic-link shell profile target was modified'

ancestor_home=$TMP_ROOT/symlink-ancestor-home
ancestor_target=$TMP_ROOT/symlink-ancestor-target
mkdir -p "$ancestor_home" "$ancestor_target"
ln -s "$ancestor_target" "$ancestor_home/.config"
if HOME="$ancestor_home" SHELL=/usr/bin/fish PATH=/usr/bin:/bin sh -c '
    . "$1"
    CUP_BIN_DIR="$HOME/.cup/bin"
    prompt_tty() {
        printf "%s\n" y
    }
    offer_path_update
' sh "$functions" >"$TMP_ROOT/symlink-ancestor.out" 2>&1; then
    fail 'shell profile below a symbolic-link directory unexpectedly succeeded'
fi
[ ! -e "$ancestor_target/fish/conf.d/cup.fish" ] ||
    fail 'shell profile was written through a symbolic-link directory'

# Stale uninstall residues are removed only when their provenance marker is valid.
residue_home=$TMP_ROOT/uninstall-residue-home
valid_residue=$residue_home/.cup-uninstall.valid
mkdir -p "$valid_residue/root/bin"
: > "$valid_residue/root/uninstall.pending"
: > "$valid_residue/root/bin/cup"
HOME="$residue_home" sh -c '. "$1"; cleanup_uninstall_residues cup' \
    sh "$functions" >/dev/null
[ ! -e "$valid_residue" ] ||
    fail 'validated uninstall residue was not removed'

invalid_residue=$residue_home/.cup-uninstall.invalid
mkdir -p "$invalid_residue/root/bin" "$invalid_residue/extra"
: > "$invalid_residue/root/uninstall.pending"
: > "$invalid_residue/root/bin/cup"
if HOME="$residue_home" sh -c \
        '. "$1"; cleanup_uninstall_residues cup' sh "$functions" \
        >"$TMP_ROOT/invalid-residue.out" 2>&1; then
    fail 'unrecognized uninstall residue was removed'
fi
[ -d "$invalid_residue" ] ||
    fail 'unrecognized uninstall residue was not preserved'
grep -F 'unrecognized uninstall residue was preserved' \
    "$TMP_ROOT/invalid-residue.out" >/dev/null ||
    fail 'unrecognized uninstall residue failure was not explained'

printf '%s\n' 'Installer metadata, transport, profile, and residue tests passed.'
