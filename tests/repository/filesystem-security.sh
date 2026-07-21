#!/bin/sh

# Purpose: Guards the closed archive domain and platform filesystem hardening.
set -eu

TESTS_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
. "$TESTS_ROOT/support/common.sh"

test_begin filesystem-security

archive_source=$PROJECT_ROOT/src/package_archive.c
format_source=$PROJECT_ROOT/src/package_archive_format.c
extract_source=$PROJECT_ROOT/src/package_extract.c
posix_source=$PROJECT_ROOT/src/system_posix.c
windows_source=$PROJECT_ROOT/src/system_windows.c
installer=$PROJECT_ROOT/scripts/install/install-cup.sh
windows_installer=$PROJECT_ROOT/scripts/install/install-cup-windows.ps1
version_script=$PROJECT_ROOT/scripts/version.sh

for value in tar.xz tar.gz zip; do
    grep -F "\"$value\"" "$format_source" >/dev/null ||
        fail "archive format parser is missing $value"
done
if grep -E 'archive_read_support_(format|filter)_all' "$archive_source" "$extract_source" >/dev/null; then
    fail 'package readers enable formats or filters outside the closed CUP domain'
fi
for token in archive_read_support_filter_gzip archive_read_support_filter_xz \
        archive_read_support_format_tar archive_read_support_format_zip \
        package_archive_reader_matches_format; do
    grep -F "$token" "$archive_source" >/dev/null ||
        fail "package archive validation is missing $token"
done

for token in openat fstatat unlinkat AT_SYMLINK_NOFOLLOW O_NOFOLLOW; do
    grep -F "$token" "$posix_source" >/dev/null ||
        fail "POSIX filesystem backend is missing $token"
done
for token in MultiByteToWideChar GetFullPathNameW FILE_ATTRIBUTE_REPARSE_POINT \
        GetNamedSecurityInfoW PROTECTED_DACL_SECURITY_INFORMATION \
        FILE_FLAG_OPEN_REPARSE_POINT; do
    grep -F "$token" "$windows_source" >/dev/null ||
        fail "Windows filesystem backend is missing $token"
done
grep -F 'L"\\\\?\\"' "$windows_source" >/dev/null ||
    fail 'Windows filesystem backend does not construct long-path prefixes'
for token in GetFullPathNameW 'L"\\\\?\\"' SetCurrentDirectoryW; do
    grep -F "$token" "$extract_source" >/dev/null ||
        fail "Windows package extraction is missing long-path support: $token"
done
grep -F '<longPathAware xmlns=\"http://schemas.microsoft.com/SMI/2016/WindowsSettings\">true</longPathAware>' \
    "$version_script" >/dev/null ||
    fail 'Windows executable manifest is not long-path aware'

for token in "umask 077" "--proto '=https'" 'verify_named_checksum' \
        'shell profile is a symbolic link'; do
    grep -F -- "$token" "$installer" >/dev/null ||
        fail "POSIX installer hardening is missing: $token"
done
for token in Assert-BaseUrl Assert-NamedChecksum Set-PrivateDirectory \
        Assert-PrivateDirectory; do
    grep -F "$token" "$windows_installer" >/dev/null ||
        fail "Windows installer hardening is missing: $token"
done

for test_name in test_package_extract.c test_package_archive.c test_filesystem.c; do
    [ -f "$PROJECT_ROOT/tests/unit/$test_name" ] ||
        fail "filesystem/archive test is missing: $test_name"
done
[ -f "$PROJECT_ROOT/tests/integration/windows/filesystem-archives.ps1" ] ||
    fail 'native Windows filesystem/archive integration suite is missing'

printf '%s\n' 'Filesystem and archive security contracts passed.'
