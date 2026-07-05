#!/bin/sh
set -eu

VERSION_FILE=${CUP_VERSION_FILE:-VERSION}

fail() {
    echo "version: $*" >&2
    exit 1
}

is_semver() {
    case "$1" in
        ''|*[!0-9.]*|.*|*..*|*.) return 1 ;;
    esac
    old_ifs=$IFS
    IFS=.
    set -- $1
    IFS=$old_ifs
    [ "$#" -eq 3 ] || return 1
    for part in "$@"; do
        case "$part" in ''|*[!0-9]*) return 1 ;; esac
        case "$part" in 0) ;; 0*) return 1 ;; esac
        [ "$part" -le 999999 ] || return 1
    done
}

base_version() {
    [ -f "$VERSION_FILE" ] || fail "missing $VERSION_FILE"
    version=$(sed -n '1p' "$VERSION_FILE" | tr -d '\r')
    [ "$(wc -l < "$VERSION_FILE" | tr -d '[:space:]')" -eq 1 ] ||
        fail "$VERSION_FILE must contain exactly one line"
    is_semver "$version" || fail "invalid semantic version '$version' in $VERSION_FILE"
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

have_git_repository() {
    command -v git >/dev/null 2>&1 &&
        git rev-parse --is-inside-work-tree >/dev/null 2>&1
}

commit_id() {
    if have_git_repository; then
        git rev-parse --short=7 HEAD 2>/dev/null || printf '%s\n' unknown
    else
        printf '%s\n' archive
    fi
}

metadata_commit_id() {
    if have_git_repository; then
        git rev-parse HEAD 2>/dev/null || printf '%s\n' unknown
    else
        printf '%s\n' archive
    fi
}

working_tree_dirty() {
    have_git_repository || return 1
    ! git diff --quiet --ignore-submodules -- 2>/dev/null ||
        ! git diff --cached --quiet --ignore-submodules -- 2>/dev/null
}

matching_tag_exists() {
    base=$1
    have_git_repository || return 1
    git rev-parse -q --verify "refs/tags/v$base^{commit}" >/dev/null 2>&1
}

at_matching_tag() {
    base=$1
    matching_tag_exists "$base" || return 1
    [ "$(git rev-parse "v$base^{commit}")" = "$(git rev-parse HEAD)" ]
}

latest_reachable_version_tag() {
    have_git_repository || return 1
    for tag in $(git tag --merged HEAD --sort=-version:refname); do
        case "$tag" in
            v*) version=${tag#v} ;;
            *) continue ;;
        esac
        if is_semver "$version"; then
            printf '%s\n' "$tag"
            return 0
        fi
    done
    return 1
}

commits_from_latest_tag() {
    if tag=$(latest_reachable_version_tag); then
        git rev-list --count "$tag..HEAD"
    else
        git rev-list --count HEAD
    fi
}

is_official_build() {
    base=$1
    [ "${CUP_RELEASE_BUILD:-0}" = 1 ] || return 1
    at_matching_tag "$base" || return 1
    ! working_tree_dirty
}

validate_release() {
    base=$(base_version)
    have_git_repository || fail "official releases require a Git checkout"
    at_matching_tag "$base" ||
        fail "HEAD must be tagged v$base and VERSION must match the tag"
    working_tree_dirty && fail "official releases require a clean working tree"
    printf '%s\n' "$base"
}

current_version() {
    base=$(base_version)

    if is_official_build "$base"; then
        printf '%s\n' "$base"
        return
    fi

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

write_if_changed() {
    destination=$1
    temporary=$2
    if [ -f "$destination" ] && cmp -s "$temporary" "$destination"; then
        rm -f "$temporary"
    else
        mv -f "$temporary" "$destination"
    fi
}

generate_files() {
    output_dir=$1
    mkdir -p "$output_dir"

    base=$(base_version)
    version=$(current_version)
    split_semver "$base"
    commit=$(commit_id)
    metadata_commit=$(metadata_commit_id)
    official=0
    is_official_build "$base" && official=1

    header_tmp="$output_dir/version.h.tmp.$$"
    cat > "$header_tmp" <<HEADER
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
    write_if_changed "$output_dir/version.h" "$header_tmp"

    metadata_tmp="$output_dir/release.txt.tmp.$$"
    cat > "$metadata_tmp" <<METADATA
format=1
version=$base
commit=$metadata_commit
METADATA
    write_if_changed "$output_dir/release.txt" "$metadata_tmp"

    resource_tmp="$output_dir/version.rc.tmp.$$"
    cat > "$resource_tmp" <<RESOURCE
#include <windows.h>

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
RESOURCE
    write_if_changed "$output_dir/version.rc" "$resource_tmp"
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

command=${1:-}
case "$command" in
    base)
        [ "$#" -eq 1 ] || usage
        base_version
        ;;
    current)
        [ "$#" -eq 1 ] || usage
        current_version
        ;;
    official)
        [ "$#" -eq 1 ] || usage
        base=$(base_version)
        if is_official_build "$base"; then printf '1\n'; else printf '0\n'; fi
        ;;
    validate-release)
        [ "$#" -eq 1 ] || usage
        validate_release
        ;;
    generate)
        [ "$#" -eq 2 ] || usage
        generate_files "$2"
        ;;
    *) usage ;;
esac
