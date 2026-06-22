#!/bin/sh
set -eu

INITIAL_VERSION=${CUP_INITIAL_VERSION:-0.1.0}
IGNORED_RELEASE_COMMIT_SUBJECT=${CUP_IGNORED_RELEASE_COMMIT_SUBJECT:-Update certs}

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
    done
    return 0
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

latest_release_tag() {
    if ! have_git_repository; then
        return 1
    fi

    git describe --tags --abbrev=0 --match 'v[0-9]*.[0-9]*.[0-9]*' HEAD 2>/dev/null || return 1
}

release_base() {
    tag=$(latest_release_tag 2>/dev/null || true)
    if [ -n "$tag" ]; then
        base=${tag#v}
        is_semver "$base" || fail "release tag '$tag' is not v<major>.<minor>.<patch>"
        printf '%s\n' "$base"
    else
        is_semver "$INITIAL_VERSION" || fail "invalid CUP_INITIAL_VERSION '$INITIAL_VERSION'"
        printf '%s\n' "$INITIAL_VERSION"
    fi
}

commit_id() {
    if have_git_repository; then
        git rev-parse --short=12 HEAD 2>/dev/null || printf '%s\n' unknown
    else
        printf '%s\n' archive
    fi
}

commits_since_release() {
    if ! have_git_repository; then
        printf '%s\n' 0
        return
    fi

    tag=$(latest_release_tag 2>/dev/null || true)
    range=HEAD
    if [ -n "$tag" ]; then
        range="$tag..HEAD"
    fi

    git log --first-parent --format='%s' "$range" | awk \
        -v ignored="$IGNORED_RELEASE_COMMIT_SUBJECT" '
            $0 != ignored { count++ }
            END { print count + 0 }
        '
}

working_tree_dirty() {
    if ! have_git_repository; then
        return 1
    fi
    ! git diff --quiet --ignore-submodules -- 2>/dev/null ||
        ! git diff --cached --quiet --ignore-submodules -- 2>/dev/null
}

current_version() {
    if [ -n "${CUP_VERSION_OVERRIDE:-}" ]; then
        is_semver "$CUP_VERSION_OVERRIDE" ||
            fail "CUP_VERSION_OVERRIDE must be <major>.<minor>.<patch>"
        printf '%s\n' "$CUP_VERSION_OVERRIDE"
        return
    fi

    base=$(release_base)
    commit=$(commit_id)
    distance=$(commits_since_release)
    exact=0

    if have_git_repository; then
        tag=$(latest_release_tag 2>/dev/null || true)
        if [ -n "$tag" ] && [ "$(git rev-parse "$tag^{commit}")" = "$(git rev-parse HEAD)" ] &&
            ! working_tree_dirty; then
            exact=1
        fi
    fi

    if [ "$exact" -eq 1 ]; then
        printf '%s\n' "$base"
        return
    fi

    if [ "$commit" = archive ]; then
        printf '%s-dev+archive\n' "$base"
    elif working_tree_dirty; then
        printf '%s-dev.%s+g%s.dirty\n' "$base" "$distance" "$commit"
    else
        printf '%s-dev.%s+g%s\n' "$base" "$distance" "$commit"
    fi
}

next_version() {
    increment=$1
    base=$(release_base)
    split_semver "$base"

    case "$increment" in
        patch)
            VERSION_PATCH=$((VERSION_PATCH + 1))
            ;;
        minor)
            VERSION_MINOR=$((VERSION_MINOR + 1))
            VERSION_PATCH=0
            ;;
        major)
            VERSION_MAJOR=$((VERSION_MAJOR + 1))
            VERSION_MINOR=0
            VERSION_PATCH=0
            ;;
        *)
            fail "unknown increment '$increment' (expected patch, minor or major)"
            ;;
    esac

    printf '%s.%s.%s\n' "$VERSION_MAJOR" "$VERSION_MINOR" "$VERSION_PATCH"
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

    version=$(current_version)
    base=$(release_base)
    if is_semver "$version"; then
        base=$version
        is_release=1
    else
        is_release=0
    fi
    split_semver "$base"
    commit=$(commit_id)

    header_tmp="$output_dir/version.h.tmp.$$"
    cat > "$header_tmp" <<HEADER
#ifndef CUP_GENERATED_VERSION_H
#define CUP_GENERATED_VERSION_H

#define CUP_VERSION "$version"
#define CUP_VERSION_BASE "$base"
#define CUP_VERSION_COMMIT "$commit"
#define CUP_VERSION_IS_RELEASE $is_release
#define CUP_VERSION_MAJOR $VERSION_MAJOR
#define CUP_VERSION_MINOR $VERSION_MINOR
#define CUP_VERSION_PATCH $VERSION_PATCH

#endif /* CUP_GENERATED_VERSION_H */
HEADER
    write_if_changed "$output_dir/version.h" "$header_tmp"

    metadata_tmp="$output_dir/release.txt.tmp.$$"
    cat > "$metadata_tmp" <<METADATA
format=1
version=$version
commit=$commit
METADATA
    write_if_changed "$output_dir/release.txt" "$metadata_tmp"

    resource_tmp="$output_dir/version.rc.tmp.$$"
    cat > "$resource_tmp" <<RESOURCE
#include <windows.h>

1 VERSIONINFO
FILEVERSION $VERSION_MAJOR,$VERSION_MINOR,$VERSION_PATCH,0
PRODUCTVERSION $VERSION_MAJOR,$VERSION_MINOR,$VERSION_PATCH,0
FILEFLAGSMASK 0x3fL
#ifdef _DEBUG
FILEFLAGS VS_FF_DEBUG
#else
FILEFLAGS 0x0L
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

print_plan() {
    mode=$1
    publish=0
    version=$(current_version)
    tag=
    distance=$(commits_since_release)
    latest=$(latest_release_tag 2>/dev/null || true)

    case "$mode" in
        auto)
            if [ -z "$latest" ]; then
                publish=1
                version=$INITIAL_VERSION
            elif [ "$distance" -ge 10 ]; then
                publish=1
                version=$(next_version patch)
            fi
            ;;
        minor|major)
            publish=1
            version=$(next_version "$mode")
            ;;
        none)
            ;;
        *)
            fail "unknown release plan '$mode'"
            ;;
    esac

    if [ "$publish" -eq 1 ]; then
        tag="v$version"
    fi

    printf 'publish=%s\n' "$publish"
    printf 'version=%s\n' "$version"
    printf 'tag=%s\n' "$tag"
    printf 'commits_since_release=%s\n' "$distance"
}

usage() {
    cat >&2 <<'USAGE'
Usage:
  scripts/version.sh current
  scripts/version.sh base
  scripts/version.sh next <patch|minor|major>
  scripts/version.sh distance
  scripts/version.sh plan <auto|minor|major|none>
  scripts/version.sh generate <output-directory>
USAGE
    exit 2
}

command=${1:-}
case "$command" in
    current)
        [ "$#" -eq 1 ] || usage
        current_version
        ;;
    base)
        [ "$#" -eq 1 ] || usage
        release_base
        ;;
    next)
        [ "$#" -eq 2 ] || usage
        next_version "$2"
        ;;
    distance)
        [ "$#" -eq 1 ] || usage
        commits_since_release
        ;;
    plan)
        [ "$#" -eq 2 ] || usage
        print_plan "$2"
        ;;
    generate)
        [ "$#" -eq 2 ] || usage
        generate_files "$2"
        ;;
    *)
        usage
        ;;
esac
