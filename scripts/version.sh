#!/bin/sh

# Validates VERSION and derives development or official build metadata from Git.
# Commands generate version.h, release.txt and the Windows VERSIONINFO resource.
set -eu

LC_ALL=C
LANG=C
TZ=UTC
export LC_ALL LANG TZ
umask 022

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
PROJECT_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd -P)
# shellcheck source=lib/path-safety.sh
. "$PROJECT_ROOT/scripts/lib/path-safety.sh"
case "${CUP_VERSION_FILE:-VERSION}" in
    /*) VERSION_FILE=${CUP_VERSION_FILE:-VERSION} ;;
    *) VERSION_FILE=$PROJECT_ROOT/${CUP_VERSION_FILE:-VERSION} ;;
esac

fail() {
    printf 'version: %s\n' "$*" >&2
    exit 1
}

validate_build_context() {
    case "${CUP_OFFICIAL_BUILD:-0}" in
        0|1)
            ;;
        *)
            fail "CUP_OFFICIAL_BUILD must be 0 or 1"
            ;;
    esac
    case "${CUP_BUILD_CONFIGURATION:-development}" in
        development|debug|coverage|sanitizers|release)
            ;;
        *)
            fail "invalid CUP_BUILD_CONFIGURATION '${CUP_BUILD_CONFIGURATION}'"
            ;;
    esac
    if [ "${CUP_OFFICIAL_BUILD:-0}" = 1 ] &&
        [ "${CUP_BUILD_CONFIGURATION:-development}" != release ]; then
        fail "official identity requires the release build configuration"
    fi
}

# VERSION and Git identity helpers.
is_semver() {
    case "$1" in
        ''|*[!0-9.]*|.*|*..*|*.)
            return 1
            ;;
    esac
    old_ifs=$IFS
    IFS=.
    set -- $1
    IFS=$old_ifs
    [ "$#" -eq 3 ] || return 1
    for part in "$@"; do
        case "$part" in
            '' | *[!0-9]*)
                return 1
                ;;
        esac
        case "$part" in
            0)
                ;;
            0*)
                return 1
                ;;
        esac
        [ "${#part}" -le 6 ] || return 1
        [ "$part" -le 999999 ] || return 1
    done
}

base_version() {
    cup_path_require_regular_file "$VERSION_FILE" 'VERSION file' || fail "missing or unsafe $VERSION_FILE"
    IFS= read -r version < "$VERSION_FILE" ||
        fail "$VERSION_FILE must contain one LF-terminated line"
    is_semver "$version" || fail "invalid semantic version '$version' in $VERSION_FILE"
    expected=$(mktemp "${TMPDIR:-/tmp}/cup-version.XXXXXX") ||
        fail 'could not create VERSION comparison file'
    printf '%s\n' "$version" > "$expected" || {
        rm -f -- "$expected"
        fail 'could not prepare VERSION comparison'
    }
    if ! cmp -s "$expected" "$VERSION_FILE"; then
        rm -f -- "$expected"
        fail "$VERSION_FILE must contain exactly one canonical LF-terminated semantic version"
    fi
    rm -f -- "$expected"
    printf '%s\n' "$version"
}

split_semver() {
    is_semver "$1" || fail "invalid semantic version '$1'"
    old_ifs=$IFS
    IFS=.
    set -- $1
    IFS=$old_ifs
    VERSION_MAJOR=$1
    VERSION_MINOR=$2
    VERSION_PATCH=$3
}

git_at_root() {
    git -C "$PROJECT_ROOT" "$@"
}

have_git_repository() {
    command -v git >/dev/null 2>&1 &&
        git_at_root rev-parse --is-inside-work-tree >/dev/null 2>&1
}

commit_id() {
    if have_git_repository; then
        git_at_root rev-parse --short=7 HEAD 2>/dev/null || printf '%s\n' unknown
    else
        printf '%s\n' archive
    fi
}

metadata_commit_id() {
    if have_git_repository; then
        git_at_root rev-parse HEAD 2>/dev/null || printf '%s\n' unknown
    else
        # release.txt keeps one fixed hexadecimal schema. The all-zero value is
        # reserved for development builds generated from a source archive;
        # official builds still require a real checkout commit.
        printf '%040d\n' 0
    fi
}

working_tree_dirty() {
    have_git_repository || return 1
    [ -n "$(git_at_root -c core.fileMode=false status --porcelain=v1 \
        --untracked-files=normal --ignore-submodules=none 2>/dev/null)" ]
}

matching_tag_exists() {
    base=$1
    have_git_repository || return 1
    git_at_root rev-parse -q --verify "refs/tags/v$base^{commit}" >/dev/null 2>&1
}

at_matching_tag() {
    base=$1
    matching_tag_exists "$base" || return 1
    [ "$(git_at_root rev-parse "v$base^{commit}")" = "$(git_at_root rev-parse HEAD)" ]
}

latest_reachable_version_tag() {
    have_git_repository || return 1
    for tag in $(git_at_root tag --merged HEAD --sort=-version:refname); do
        case "$tag" in v*) version=${tag#v} ;; *) continue ;; esac
        if is_semver "$version"; then
            printf '%s\n' "$tag"
            return 0
        fi
    done
    return 1
}

commits_from_latest_tag() {
    if tag=$(latest_reachable_version_tag); then
        git_at_root rev-list --count "$tag..HEAD"
    else
        git_at_root rev-list --count HEAD
    fi
}

explicit_release_context_present() {
    [ -n "${CUP_RELEASE_VERSION:-}${CUP_RELEASE_TAG:-}${CUP_RELEASE_COMMIT:-}" ]
}

validate_explicit_release_context() {
    base=$1
    [ -n "${CUP_RELEASE_VERSION:-}" ] && [ -n "${CUP_RELEASE_TAG:-}" ] &&
        [ -n "${CUP_RELEASE_COMMIT:-}" ] ||
        fail 'CUP_RELEASE_VERSION, CUP_RELEASE_TAG and CUP_RELEASE_COMMIT must be provided together'
    [ "$CUP_RELEASE_VERSION" = "$base" ] ||
        fail "release version '$CUP_RELEASE_VERSION' does not match VERSION '$base'"
    [ "$CUP_RELEASE_TAG" = "v$base" ] ||
        fail "release tag must be v$base"
    case "$CUP_RELEASE_COMMIT" in
        ''|*[!0-9a-f]*) fail 'release commit must be a lowercase hexadecimal Git commit' ;;
    esac
    [ "${#CUP_RELEASE_COMMIT}" -eq 40 ] || fail 'release commit must contain 40 hexadecimal characters'
    [ "$(git_at_root rev-parse HEAD)" = "$CUP_RELEASE_COMMIT" ] ||
        fail 'release commit does not match checkout HEAD'
}

validate_official_build() {
    base=$1
    [ "${CUP_OFFICIAL_BUILD:-0}" = 1 ] || fail 'official build identity was not requested'
    [ "${CUP_BUILD_CONFIGURATION:-development}" = release ] ||
        fail 'official identity requires the release build configuration'
    have_git_repository || fail 'official releases require a Git checkout'
    if explicit_release_context_present; then
        validate_explicit_release_context "$base"
    else
        at_matching_tag "$base" ||
            fail "HEAD must be tagged v$base when no explicit release context is provided"
    fi
    if working_tree_dirty; then
        fail 'official releases require a clean working tree, including untracked files'
    fi
    return 0
}

is_official_build() {
    base=$1
    [ "${CUP_OFFICIAL_BUILD:-0}" = 1 ] || return 1
    validate_official_build "$base"
}

validate_release() {
    base=$(base_version)
    validate_official_build "$base"
    printf '%s\n' "$base"
}

# Development and official version derivation.
development_version() {
    base=$1
    if ! have_git_repository; then
        printf '%s-dev+archive\n' "$base"
        return
    fi

    distance=$(commits_from_latest_tag)
    commit=$(commit_id)
    suffix=
    working_tree_dirty && suffix=.dirty
    printf '%s-dev.%s+%s%s\n' "$base" "$distance" "$commit" "$suffix"
}

current_version() {
    base=$(base_version)
    if is_official_build "$base"; then
        printf '%s\n' "$base"
    else
        development_version "$base"
    fi
}

write_if_changed() {
    destination=$1
    if [ -n "${CUP_BUILD_ROOT:-}" ]; then
        cup_path_prepare_child_file "$CUP_BUILD_ROOT" "$destination" 'generated version output' || exit 1
    else
        cup_path_prepare_file_target "$destination" 'generated version output' || exit 1
    fi
    cup_path_write_file "$destination" 0644 if-different
}

# Deterministic generated metadata outputs.
generate_files() {
    output_dir=$1
    case "$output_dir" in /*|[A-Za-z]:/*) ;; *) output_dir=$(pwd -P)/$output_dir ;; esac
    if [ -n "${CUP_BUILD_ROOT:-}" ]; then
        cup_path_require_build_root "$CUP_BUILD_ROOT" || exit 1
    fi

    # Resolve and validate build identity before creating output directories. An invalid official
    # request must have no filesystem side effect.
    base=$(base_version)
    official=0
    if is_official_build "$base"; then
        official=1
        version=$base
    else
        version=$(development_version "$base")
    fi
    split_semver "$base"
    commit=$(commit_id)
    metadata_commit=$(metadata_commit_id)

    if [ -n "${CUP_BUILD_ROOT:-}" ]; then
        cup_path_prepare_child_directory "$CUP_BUILD_ROOT" "$output_dir" 'generated version directory' || exit 1
    else
        cup_path_prepare_directory_chain "$output_dir" 'generated version directory' || exit 1
    fi

    cat <<HEADER | write_if_changed "$output_dir/version.h"
#ifndef CUP_GENERATED_VERSION_H
#define CUP_GENERATED_VERSION_H

#define CUP_VERSION "$version"
#define CUP_VERSION_BASE "$base"
#define CUP_VERSION_COMMIT "$commit"
#define CUP_VERSION_OFFICIAL $official
#define CUP_VERSION_MAJOR $VERSION_MAJOR
#define CUP_VERSION_MINOR $VERSION_MINOR
#define CUP_VERSION_PATCH $VERSION_PATCH

#endif /* CUP_GENERATED_VERSION_H */
HEADER

    cat <<METADATA | write_if_changed "$output_dir/release.txt"
format=1
version=$base
commit=$metadata_commit
METADATA

    cat <<RESOURCE | write_if_changed "$output_dir/version.rc"
#include <windows.h>
#include "version.h"

1 VERSIONINFO
FILEVERSION $VERSION_MAJOR,$VERSION_MINOR,$VERSION_PATCH,0
PRODUCTVERSION $VERSION_MAJOR,$VERSION_MINOR,$VERSION_PATCH,0
FILEFLAGSMASK 0x3fL
#if CUP_VERSION_OFFICIAL
FILEFLAGS 0x0L
#else
FILEFLAGS VS_FF_PRERELEASE | VS_FF_PRIVATEBUILD
#endif
FILEOS VOS_NT_WINDOWS32
FILETYPE VFT_APP
FILESUBTYPE VFT2_UNKNOWN
BEGIN
    BLOCK "StringFileInfo"
    BEGIN
        BLOCK "040904b0"
        BEGIN
            VALUE "CompanyName", "coffee-clang"
            VALUE "FileDescription", "cup toolchain manager"
            VALUE "FileVersion", "$version"
            VALUE "InternalName", "cup"
            VALUE "OriginalFilename", "cup.exe"
            VALUE "ProductName", "cup"
            VALUE "ProductVersion", "$version"
        END
    END
    BLOCK "VarFileInfo"
    BEGIN
        VALUE "Translation", 0x0409, 1200
    END
END

1 24
BEGIN
    "<?xml version=""1.0"" encoding=""UTF-8"" standalone=""yes""?>\r\n"
    "<assembly xmlns=""urn:schemas-microsoft-com:asm.v1"" manifestVersion=""1.0"">\r\n"
    "  <application xmlns=""urn:schemas-microsoft-com:asm.v3"">\r\n"
    "    <windowsSettings>\r\n"
    "      <longPathAware xmlns=""http://schemas.microsoft.com/SMI/2016/WindowsSettings"">true</longPathAware>\r\n"
    "    </windowsSettings>\r\n"
    "  </application>\r\n"
    "</assembly>\r\n"
END
RESOURCE
}

usage() {
    cat >&2 <<'USAGE'
Usage:
  scripts/version.sh base
  scripts/version.sh current
  scripts/version.sh official
  scripts/version.sh validate-release
  scripts/version.sh generate <output-directory>
USAGE
    exit 2
}

# Command dispatch.
command=${1:-}
case "$command" in
    base)
        [ "$#" -eq 1 ] || usage
        base_version
        ;;
    current)
        [ "$#" -eq 1 ] || usage
        validate_build_context
        current_version
        ;;
    official)
        [ "$#" -eq 1 ] || usage
        validate_build_context
        base=$(base_version)
        if is_official_build "$base"; then
            printf '1\n'
        else
            printf '0\n'
        fi
        ;;
    validate-release)
        [ "$#" -eq 1 ] || usage
        validate_build_context
        validate_release
        ;;
    generate)
        [ "$#" -eq 2 ] || usage
        validate_build_context
        generate_files "$2"
        ;;
    *)
        usage
        ;;
esac
