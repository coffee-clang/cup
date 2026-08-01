#!/usr/bin/env sh

# Purpose: Installs one immutable official cup bootstrap on POSIX, or delegates
# Windows shells to PowerShell.
# The generated release version, tag and commit select all downloaded assets.
set -eu
umask 077

REPO_OWNER="coffee-clang"
REPO_NAME="cup"
CUP_RELEASE_VERSION="@CUP_RELEASE_VERSION@"
CUP_RELEASE_TAG="@CUP_RELEASE_TAG@"
CUP_RELEASE_COMMIT="@CUP_RELEASE_COMMIT@"
DEFAULT_BASE_URL="https://github.com/${REPO_OWNER}/${REPO_NAME}/releases/download/\
${CUP_RELEASE_TAG}"
BASE_URL="${CUP_INSTALL_BASE_URL:-$DEFAULT_BASE_URL}"
BASE_URL="${BASE_URL%/}"

CUP_ROOT=""
CUP_BIN_DIR=""
CUP_CONFIG_DIR=""
CUP_HELPERS_DIR=""
PACKAGES_CFG=""
INSTALL_CONFIG=""
COMMON_CHECKSUMS=""
PLATFORM_CHECKSUMS=""
UNINSTALL_SCRIPT=""
UPDATE_HELPER=""
ROOT_MARKER=""
CUP_AVAILABLE_IN_PATH=0
WINDOWS_SHELL_INSTALL=0

fail() {
    printf 'Error: %s\n' "$*" >&2
    exit 1
}
info() {
    printf '%s\n' "$*"
}

# User-facing installers intentionally avoid awk and non-standard find options.
# These helpers parse the small fixed release formats with POSIX shell builtins.
read_exact_one_line() {
    file=$1
    FILE_LINE_1=
    FILE_EXTRA_LINE=
    {
        IFS= read -r FILE_LINE_1 || return 1
        if IFS= read -r FILE_EXTRA_LINE; then
            return 1
        fi
    } < "$file"
}

read_exact_three_lines() {
    file=$1
    FILE_LINE_1=
    FILE_LINE_2=
    FILE_LINE_3=
    FILE_EXTRA_LINE=
    {
        IFS= read -r FILE_LINE_1 || return 1
        IFS= read -r FILE_LINE_2 || return 1
        IFS= read -r FILE_LINE_3 || return 1
        if IFS= read -r FILE_EXTRA_LINE; then
            return 1
        fi
    } < "$file"
}

read_exact_seven_lines() {
    file=$1
    FILE_LINE_1=
    FILE_LINE_2=
    FILE_LINE_3=
    FILE_LINE_4=
    FILE_LINE_5=
    FILE_LINE_6=
    FILE_LINE_7=
    FILE_EXTRA_LINE=
    {
        IFS= read -r FILE_LINE_1 || return 1
        IFS= read -r FILE_LINE_2 || return 1
        IFS= read -r FILE_LINE_3 || return 1
        IFS= read -r FILE_LINE_4 || return 1
        IFS= read -r FILE_LINE_5 || return 1
        IFS= read -r FILE_LINE_6 || return 1
        IFS= read -r FILE_LINE_7 || return 1
        if IFS= read -r FILE_EXTRA_LINE; then
            return 1
        fi
    } < "$file"
}

read_first_line() {
    FIRST_LINE=
    IFS= read -r FIRST_LINE < "$1" || [ -n "$FIRST_LINE" ]
}

is_hex_length() {
    value=$1
    expected_length=$2
    [ "${#value}" -eq "$expected_length" ] || return 1
    case "$value" in
        *[!0-9a-fA-F]*) return 1 ;;
    esac
}

is_lower_hex_range() {
    value=$1
    minimum_length=$2
    maximum_length=$3
    [ "${#value}" -ge "$minimum_length" ] &&
        [ "${#value}" -le "$maximum_length" ] || return 1
    case "$value" in
        *[!0-9a-f]*) return 1 ;;
    esac
}

is_version_part() {
    value=$1
    case "$value" in
        ''|*[!0-9]*) return 1 ;;
        0) return 0 ;;
        0*) return 1 ;;
    esac
    [ "${#value}" -le 6 ] || return 1
    [ "$value" -le 999999 ] 2>/dev/null
}

is_release_version() {
    value=$1
    case "$value" in
        *.*.*) ;;
        *) return 1 ;;
    esac
    major=${value%%.*}
    remainder=${value#*.}
    minor=${remainder%%.*}
    patch=${remainder#*.}
    case "$patch" in
        *.*) return 1 ;;
    esac
    is_version_part "$major" &&
        is_version_part "$minor" &&
        is_version_part "$patch"
}

parse_checksum_line() {
    checksum_line=$1
    CHECKSUM_HASH=
    CHECKSUM_NAME=
    CHECKSUM_EXTRA=
    old_ifs=$IFS
    IFS=' 	'
    read -r CHECKSUM_HASH CHECKSUM_NAME CHECKSUM_EXTRA <<EOF
$checksum_line
EOF
    IFS=$old_ifs

    [ -n "$CHECKSUM_HASH" ] && [ -n "$CHECKSUM_NAME" ] &&
        [ -z "$CHECKSUM_EXTRA" ] || return 1
    is_hex_length "$CHECKSUM_HASH" 64 || return 1
    case "$CHECKSUM_NAME" in
        \*) CHECKSUM_NAME=${CHECKSUM_NAME#\*} ;;
    esac
    [ -n "$CHECKSUM_NAME" ] || return 1
}

file_contains_line() {
    expected_line=$1
    file=$2
    current_line=
    while IFS= read -r current_line || [ -n "$current_line" ]; do
        [ "$current_line" = "$expected_line" ] && return 0
    done < "$file"
    return 1
}

directory_entry_count() {
    directory=$1
    DIRECTORY_ENTRY_COUNT=0
    for entry in "$directory"/* "$directory"/.[!.]* "$directory"/..?*; do
        [ -e "$entry" ] || [ -L "$entry" ] || continue
        DIRECTORY_ENTRY_COUNT=$((DIRECTORY_ENTRY_COUNT + 1))
    done
}

# Validate the generated installer identity before any network request.
validate_installer_identity() {
    placeholder_marker='@''CUP_RELEASE_'
    case "$CUP_RELEASE_VERSION:$CUP_RELEASE_TAG:$CUP_RELEASE_COMMIT" in
        *"$placeholder_marker"*)
            fail "installer was not prepared for a concrete release"
            ;;
    esac
    is_release_version "$CUP_RELEASE_VERSION" ||
        fail "installer has an invalid release version"
    [ "$CUP_RELEASE_TAG" = "v$CUP_RELEASE_VERSION" ] ||
        fail "installer release tag does not match its version"
    is_lower_hex_range "$CUP_RELEASE_COMMIT" 40 40 ||
        fail "installer has an invalid release commit"
}
validate_base_url() {
    case "$BASE_URL" in
        *://*@*)
            fail "installer base URL must not contain credentials"
            ;;
    esac
    case "$BASE_URL" in
        https://*) ;;
        http://127.0.0.1:*|http://localhost:*)
            [ "${CUP_INSTALL_ALLOW_INSECURE:-0}" = 1 ] ||
                fail "HTTP is allowed only for an explicit loopback test"
            ;;
        *)
            fail "installer base URL must use HTTPS"
            ;;
    esac
}

need_command() {
    command -v "$1" >/dev/null 2>&1 ||
        fail "required command not found: $1"
}

require_shell_install_commands() {
    for required_command in chmod cp mkdir mktemp mv rm; do
        need_command "$required_command"
    done
}

has_tty() {
    ( : </dev/tty && : >/dev/tty ) 2>/dev/null
}

prompt_tty() {
    prompt="$1"
    default_value="$2"
    if has_tty; then
        printf '%s' "$prompt" > /dev/tty
        IFS= read -r answer < /dev/tty || answer="$default_value"
        printf '%s\n' "$answer"
    else
        printf 'No interactive terminal available; using default: %s\n' "$default_value" >&2
        printf '%s\n' "$default_value"
    fi
}

# Persistent root selection. A marker owns a root; complete legacy CUP layouts are adopted once.
root_marker_is_valid() {
    candidate=$1
    marker="$candidate/root.txt"

    [ -f "$marker" ] && [ ! -L "$marker" ] || return 1
    read_exact_three_lines "$marker" || return 1
    [ "$FILE_LINE_1" = "format=1" ] &&
        [ "$FILE_LINE_2" = "product=coffee-clang/cup" ] &&
        [ "$FILE_LINE_3" = "layout=1" ]
}

checksum_file_has_exact_entries() {
    checksum_file=$1
    shift
    [ -s "$checksum_file" ] && [ ! -L "$checksum_file" ] || return 1

    entry_count=0
    checksum_line=
    while IFS= read -r checksum_line || [ -n "$checksum_line" ]; do
        [ -n "$checksum_line" ] && parse_checksum_line "$checksum_line" || return 1
        entry_count=$((entry_count + 1))
    done < "$checksum_file"
    [ "$entry_count" -eq "$#" ] || return 1

    for expected in "$@"; do
        matches=0
        checksum_line=
        while IFS= read -r checksum_line || [ -n "$checksum_line" ]; do
            parse_checksum_line "$checksum_line" || return 1
            [ "$CHECKSUM_NAME" = "$expected" ] && matches=$((matches + 1))
        done < "$checksum_file"
        [ "$matches" -eq 1 ] || return 1
    done
}

file_sha256() {
    file=$1
    FILE_SHA256=
    hash_output=
    if command -v sha256sum >/dev/null 2>&1; then
        IFS=' ' read -r FILE_SHA256 hash_output <<EOF_HASH
$(sha256sum "$file" 2>/dev/null || true)
EOF_HASH
    elif command -v shasum >/dev/null 2>&1; then
        IFS=' ' read -r FILE_SHA256 hash_output <<EOF_HASH
$(shasum -a 256 "$file" 2>/dev/null || true)
EOF_HASH
    else
        return 1
    fi
    is_hex_length "$FILE_SHA256" 64
}

checksum_expected_hash() {
    checksum_file=$1
    expected_name=$2
    EXPECTED_HASH=
    matches=0
    checksum_line=
    while IFS= read -r checksum_line || [ -n "$checksum_line" ]; do
        parse_checksum_line "$checksum_line" || return 1
        if [ "$CHECKSUM_NAME" = "$expected_name" ]; then
            matches=$((matches + 1))
            EXPECTED_HASH=$CHECKSUM_HASH
        fi
    done < "$checksum_file"
    [ "$matches" -eq 1 ]
}

legacy_checksum_matches() {
    checksum_file=$1
    expected_name=$2
    actual_file=$3
    [ -f "$actual_file" ] && [ ! -L "$actual_file" ] || return 1
    checksum_expected_hash "$checksum_file" "$expected_name" || return 1
    file_sha256 "$actual_file" || return 1
    [ "$FILE_SHA256" = "$EXPECTED_HASH" ]
}

trim_document_value() {
    TRIMMED_VALUE=$1
    tab=$(printf '\t')
    while :; do
        case "$TRIMMED_VALUE" in
            ' '*) TRIMMED_VALUE=${TRIMMED_VALUE# } ;;
            "$tab"*) TRIMMED_VALUE=${TRIMMED_VALUE#"$tab"} ;;
            *) break ;;
        esac
    done
    while :; do
        case "$TRIMMED_VALUE" in
            *' ') TRIMMED_VALUE=${TRIMMED_VALUE% } ;;
            *"$tab") TRIMMED_VALUE=${TRIMMED_VALUE%"$tab"} ;;
            *) break ;;
        esac
    done
}

parse_document_line() {
    DOCUMENT_KEY=
    DOCUMENT_VALUE=
    current_line=$1
    carriage_return=$(printf '\r')
    case "$current_line" in
        *"$carriage_return") current_line=${current_line%?} ;;
    esac
    trim_document_value "$current_line"
    current_line=$TRIMMED_VALUE
    case "$current_line" in
        ''|'#'*) return 2 ;;
        *=*) DOCUMENT_KEY=${current_line%%=*}; DOCUMENT_VALUE=${current_line#*=} ;;
        *) return 1 ;;
    esac
    trim_document_value "$DOCUMENT_KEY"
    DOCUMENT_KEY=$TRIMMED_VALUE
    trim_document_value "$DOCUMENT_VALUE"
    DOCUMENT_VALUE=$TRIMMED_VALUE
    [ -n "$DOCUMENT_KEY" ] && [ -n "$DOCUMENT_VALUE" ]
}

safe_identifier_is_valid() {
    identifier=$1
    [ -n "$identifier" ] && [ "${#identifier}" -lt 128 ] || return 1
    case "$identifier" in
        [a-zA-Z0-9]* ) ;;
        *) return 1 ;;
    esac
    case "$identifier" in
        *[!a-zA-Z0-9._+-]*) return 1 ;;
        .|..|*[/\\:]*) return 1 ;;
    esac
}

canonical_name_is_valid() {
    safe_identifier_is_valid "$1" || return 1
    case "$1" in *[A-Z]*) return 1 ;; esac
}

supported_platform_is_valid() {
    case "$1" in
        linux-x64|linux-arm64|windows-x64|macos-x64|macos-arm64) return 0 ;;
    esac
    return 1
}

supported_component_is_valid() {
    case "$1" in
        compiler|debugger|linker|formatter|linter|language-server|analyzer) return 0 ;;
    esac
    return 1
}

tool_component() {
    TOOL_COMPONENT=
    case "$1" in
        gcc|clang) TOOL_COMPONENT=compiler ;;
        gdb|lldb) TOOL_COMPONENT=debugger ;;
        lld|ld) TOOL_COMPONENT=linker ;;
        clang-format) TOOL_COMPONENT=formatter ;;
        clang-tidy) TOOL_COMPONENT=linter ;;
        clangd) TOOL_COMPONENT=language-server ;;
        valgrind) TOOL_COMPONENT=analyzer ;;
        *) return 1 ;;
    esac
}

tool_matches_component() {
    tool_component "$2" && [ "$TOOL_COMPONENT" = "$1" ]
}

split_scope_key() {
    scope=$1
    SCOPE_COMPONENT=${scope%%.*}
    remainder=${scope#*.}
    [ "$remainder" != "$scope" ] || return 1
    SCOPE_HOST=${remainder%%.*}
    SCOPE_TARGET=${remainder#*.}
    [ "$SCOPE_TARGET" != "$remainder" ] || return 1
    case "$SCOPE_TARGET" in *.*) return 1 ;; esac
    supported_component_is_valid "$SCOPE_COMPONENT" &&
        supported_platform_is_valid "$SCOPE_HOST" &&
        supported_platform_is_valid "$SCOPE_TARGET"
}

split_policy_scope_key() {
    scope=$1
    POLICY_HOST=${scope%%.*}
    remainder=${scope#*.}
    [ "$remainder" != "$scope" ] || return 1
    POLICY_TARGET=${remainder%%.*}
    POLICY_COMPONENT=${remainder#*.}
    [ "$POLICY_COMPONENT" != "$remainder" ] || return 1
    case "$POLICY_COMPONENT" in *.*) return 1 ;; esac
    supported_platform_is_valid "$POLICY_HOST" &&
        supported_platform_is_valid "$POLICY_TARGET" &&
        supported_component_is_valid "$POLICY_COMPONENT"
}

split_catalog_key() {
    key=$1
    CATALOG_COMPONENT=${key%%.*}
    remainder=${key#*.}
    [ "$remainder" != "$key" ] || return 1
    CATALOG_TOOL=${remainder%%.*}
    remainder=${remainder#*.}
    CATALOG_HOST=${remainder%%.*}
    remainder=${remainder#*.}
    CATALOG_TARGET=${remainder%%.*}
    CATALOG_FIELD=${remainder#*.}
    [ "$CATALOG_FIELD" != "$remainder" ] || return 1
    case "$CATALOG_FIELD" in *.*) return 1 ;; esac
    supported_component_is_valid "$CATALOG_COMPONENT" &&
        tool_matches_component "$CATALOG_COMPONENT" "$CATALOG_TOOL" &&
        supported_platform_is_valid "$CATALOG_HOST" &&
        supported_platform_is_valid "$CATALOG_TARGET"
}

validate_identifier_list() {
    list_value=$1
    list_kind=$2
    expected=${3:-}
    LIST_CONTAINS=0
    LIST_COMPONENTS='|'
    seen_items='|'
    remaining=$list_value
    item_count=0
    while :; do
        case "$remaining" in
            *,*) item=${remaining%%,*}; remaining=${remaining#*,}; more=1 ;;
            *) item=$remaining; more=0 ;;
        esac
        trim_document_value "$item"
        item=$TRIMMED_VALUE
        canonical_name_is_valid "$item" || return 1
        case "$seen_items" in *"|$item|"*) return 1 ;; esac
        seen_items="$seen_items$item|"
        item_count=$((item_count + 1))
        [ "$item" = "$expected" ] && LIST_CONTAINS=1
        case "$list_kind" in
            identifier) ;;
            component) supported_component_is_valid "$item" || return 1 ;;
            tool)
                tool_component "$item" || return 1
                case "$LIST_COMPONENTS" in *"|$TOOL_COMPONENT|"*) return 1 ;; esac
                LIST_COMPONENTS="$LIST_COMPONENTS$TOOL_COMPONENT|"
                ;;
            format)
                case "$item" in tar.xz|tar.gz|zip) ;; *) return 1 ;; esac
                ;;
            *) return 1 ;;
        esac
        [ "$more" -eq 1 ] || break
        [ -n "$remaining" ] || return 1
    done
    [ "$item_count" -gt 0 ]
}

url_template_is_valid() {
    url=$1
    kind=$2
    case "$url" in https://*) ;; *) return 1 ;; esac
    case "$url" in *[[:space:]]*) return 1 ;; esac
    URL_SEEN_TOOL=0
    URL_SEEN_HOST=0
    URL_SEEN_TARGET=0
    URL_SEEN_VERSION=0
    URL_SEEN_FORMAT=0
    cursor=$url
    while [ -n "$cursor" ]; do
        case "$cursor" in
            \{*)
                after_open=${cursor#\{}
                case "$after_open" in *\}*) ;; *) return 1 ;; esac
                name=${after_open%%\}*}
                case "$name" in
                    tool) URL_SEEN_TOOL=1 ;;
                    host_platform) URL_SEEN_HOST=1 ;;
                    target_platform) URL_SEEN_TARGET=1 ;;
                    version) URL_SEEN_VERSION=1 ;;
                    format)
                        [ "$kind" = package ] || return 1
                        URL_SEEN_FORMAT=1
                        ;;
                    *) return 1 ;;
                esac
                cursor=${after_open#*\}}
                ;;
            \}*) return 1 ;;
            *) cursor=${cursor#?} ;;
        esac
    done
    [ "$URL_SEEN_HOST" -eq 1 ] && [ "$URL_SEEN_TARGET" -eq 1 ] &&
        [ "$URL_SEEN_VERSION" -eq 1 ] || return 1
    if [ "$kind" = package ]; then
        [ "$URL_SEEN_FORMAT" -eq 1 ]
    else
        [ "$URL_SEEN_FORMAT" -eq 0 ]
    fi
}

reset_catalog_tuple() {
    CATALOG_STABLE=
    CATALOG_VERSIONS=
    CATALOG_DEFAULT_FORMAT=
    CATALOG_FORMATS=
    CATALOG_URL=
    CATALOG_CHECKSUM_URL=
    CATALOG_STABLE_SEEN=0
    CATALOG_VERSIONS_SEEN=0
    CATALOG_DEFAULT_FORMAT_SEEN=0
    CATALOG_FORMATS_SEEN=0
    CATALOG_URL_SEEN=0
    CATALOG_CHECKSUM_URL_SEEN=0
    CATALOG_FIELD_COUNT=0
}

set_catalog_tuple_field() {
    field=$1
    field_value=$2
    case "$field" in
        stable_version)
            [ "$CATALOG_STABLE_SEEN" -eq 0 ] || return 1
            CATALOG_STABLE=$field_value
            CATALOG_STABLE_SEEN=1
            ;;
        available_versions)
            [ "$CATALOG_VERSIONS_SEEN" -eq 0 ] || return 1
            CATALOG_VERSIONS=$field_value
            CATALOG_VERSIONS_SEEN=1
            ;;
        default_format)
            [ "$CATALOG_DEFAULT_FORMAT_SEEN" -eq 0 ] || return 1
            CATALOG_DEFAULT_FORMAT=$field_value
            CATALOG_DEFAULT_FORMAT_SEEN=1
            ;;
        formats)
            [ "$CATALOG_FORMATS_SEEN" -eq 0 ] || return 1
            CATALOG_FORMATS=$field_value
            CATALOG_FORMATS_SEEN=1
            ;;
        url_template)
            [ "$CATALOG_URL_SEEN" -eq 0 ] || return 1
            CATALOG_URL=$field_value
            CATALOG_URL_SEEN=1
            ;;
        checksum_url_template)
            [ "$CATALOG_CHECKSUM_URL_SEEN" -eq 0 ] || return 1
            CATALOG_CHECKSUM_URL=$field_value
            CATALOG_CHECKSUM_URL_SEEN=1
            ;;
        *) return 1 ;;
    esac
    CATALOG_FIELD_COUNT=$((CATALOG_FIELD_COUNT + 1))
}

validate_catalog_tuple() {
    [ "$CATALOG_FIELD_COUNT" -eq 6 ] || return 1
    safe_identifier_is_valid "$CATALOG_STABLE" || return 1
    validate_identifier_list "$CATALOG_VERSIONS" identifier "$CATALOG_STABLE" || return 1
    [ "$LIST_CONTAINS" -eq 1 ] || return 1
    case "$CATALOG_DEFAULT_FORMAT" in tar.xz|tar.gz|zip) ;; *) return 1 ;; esac
    validate_identifier_list "$CATALOG_FORMATS" format "$CATALOG_DEFAULT_FORMAT" || return 1
    [ "$LIST_CONTAINS" -eq 1 ] || return 1
    url_template_is_valid "$CATALOG_URL" package || return 1
    url_template_is_valid "$CATALOG_CHECKSUM_URL" checksum
}

legacy_catalog_is_valid() {
    catalog_file=$1
    [ -f "$catalog_file" ] && [ ! -L "$catalog_file" ] && [ -s "$catalog_file" ] || return 1
    completed_prefixes='|'
    current_prefix=
    record_count=0
    reset_catalog_tuple
    catalog_line=
    while IFS= read -r catalog_line || [ -n "$catalog_line" ]; do
        if parse_document_line "$catalog_line"; then
            key=$DOCUMENT_KEY
            value=$DOCUMENT_VALUE
        else
            status=$?
            [ "$status" -eq 2 ] && continue
            return 1
        fi
        split_catalog_key "$key" || return 1
        prefix="$CATALOG_COMPONENT.$CATALOG_TOOL.$CATALOG_HOST.$CATALOG_TARGET"
        if [ "$prefix" != "$current_prefix" ]; then
            if [ -n "$current_prefix" ]; then
                validate_catalog_tuple || return 1
                completed_prefixes="$completed_prefixes$current_prefix|"
            fi
            case "$completed_prefixes" in *"|$prefix|"*) return 1 ;; esac
            current_prefix=$prefix
            reset_catalog_tuple
        fi
        set_catalog_tuple_field "$CATALOG_FIELD" "$value" || return 1
        record_count=$((record_count + 1))
    done < "$catalog_file"
    [ "$record_count" -gt 0 ] && [ -n "$current_prefix" ] || return 1
    validate_catalog_tuple
}

legacy_policy_is_valid() {
    policy_file=$1
    [ -f "$policy_file" ] && [ ! -L "$policy_file" ] && [ -s "$policy_file" ] || return 1
    seen_keys='|'
    seen_format=0
    default_count=0
    profile_count=0
    toolchain_count=0
    policy_line=
    while IFS= read -r policy_line || [ -n "$policy_line" ]; do
        if parse_document_line "$policy_line"; then
            key=$DOCUMENT_KEY
            value=$DOCUMENT_VALUE
        else
            status=$?
            [ "$status" -eq 2 ] && continue
            return 1
        fi
        case "$seen_keys" in *"|$key|"*) return 1 ;; esac
        seen_keys="$seen_keys$key|"
        case "$key" in
            format)
                [ "$seen_format" -eq 0 ] && [ "$value" = 1 ] || return 1
                seen_format=1
                ;;
            default.*)
                [ "$seen_format" -eq 1 ] || return 1
                split_policy_scope_key "${key#default.}" || return 1
                canonical_name_is_valid "$value" &&
                    tool_matches_component "$POLICY_COMPONENT" "$value" || return 1
                default_count=$((default_count + 1))
                ;;
            profile.*)
                [ "$seen_format" -eq 1 ] || return 1
                name=${key#profile.}
                canonical_name_is_valid "$name" || return 1
                validate_identifier_list "$value" component || return 1
                profile_count=$((profile_count + 1))
                ;;
            toolchain.*)
                [ "$seen_format" -eq 1 ] || return 1
                name=${key#toolchain.}
                canonical_name_is_valid "$name" || return 1
                validate_identifier_list "$value" tool || return 1
                toolchain_count=$((toolchain_count + 1))
                ;;
            *) return 1 ;;
        esac
    done < "$policy_file"
    [ "$seen_format" -eq 1 ] && [ "$default_count" -gt 0 ] &&
        [ "$profile_count" -gt 0 ] && [ "$toolchain_count" -gt 0 ]
}

legacy_state_is_valid() {
    state_file=$1
    if [ ! -e "$state_file" ] && [ ! -L "$state_file" ]; then
        return 0
    fi
    [ -f "$state_file" ] && [ ! -L "$state_file" ] && [ -s "$state_file" ] || return 1
    seen_keys='|'
    installed_records='|'
    seen_format=0
    state_line=
    while IFS= read -r state_line || [ -n "$state_line" ]; do
        if parse_document_line "$state_line"; then
            key=$DOCUMENT_KEY
            value=$DOCUMENT_VALUE
        else
            status=$?
            [ "$status" -eq 2 ] && continue
            return 1
        fi
        case "$seen_keys" in *"|$key|"*) return 1 ;; esac
        seen_keys="$seen_keys$key|"
        case "$key" in
            format)
                [ "$seen_format" -eq 0 ] && [ "$value" = 1 ] || return 1
                seen_format=1
                ;;
            installed.*|default.*)
                [ "$seen_format" -eq 1 ] || return 1
                case "$key" in
                    installed.*) record_type=installed; scope=${key#installed.} ;;
                    default.*) record_type=default; scope=${key#default.} ;;
                esac
                split_scope_key "$scope" || return 1
                case "$value" in
                    *@*) tool=${value%%@*}; version=${value#*@} ;;
                    *) return 1 ;;
                esac
                [ "${version#*@}" = "$version" ] &&
                    canonical_name_is_valid "$tool" && safe_identifier_is_valid "$version" &&
                    [ "$version" != stable ] &&
                    tool_matches_component "$SCOPE_COMPONENT" "$tool" || return 1
                if [ "$record_type" = installed ]; then
                    installed_records="$installed_records$scope=$value|"
                else
                    case "$installed_records" in *"|$scope=$value|"*) ;; *) return 1 ;; esac
                fi
                ;;
            *) return 1 ;;
        esac
    done < "$state_file"
    [ "$seen_format" -eq 1 ]
}

root_has_cup_traces() {
    candidate=$1
    installed_name=$2
    case "$installed_name" in
        *.exe) update_helper=cup-update-helper.exe; uninstall_helper=uninstall.ps1 ;;
        *) update_helper=cup-update-helper; uninstall_helper=uninstall.sh ;;
    esac
    for trace in \
        "$candidate/bin/$installed_name" \
        "$candidate/helpers/$update_helper" \
        "$candidate/helpers/$uninstall_helper" \
        "$candidate/config/SHA256SUMS.common" \
        "$candidate/state.txt"; do
        if [ -e "$trace" ] || [ -L "$trace" ]; then
            return 0
        fi
    done
    return 1
}

root_has_cup_binary() {
    candidate=$1
    installed_name=$2
    [ -e "$candidate/bin/$installed_name" ] ||
        [ -L "$candidate/bin/$installed_name" ]
}

legacy_root_is_valid() {
    candidate=$1
    installed_name=$2
    platform=$3
    case "$installed_name" in
        *.exe)
            update_helper=cup-update-helper.exe
            uninstall_helper=uninstall.ps1
            cup_asset="cup-$platform.exe"
            ;;
        *)
            update_helper=cup-update-helper
            uninstall_helper=uninstall.sh
            cup_asset="cup-$platform"
            ;;
    esac

    for directory in bin components staging cache config helpers; do
        [ -d "$candidate/$directory" ] && [ ! -L "$candidate/$directory" ] || return 1
    done
    for asset in \
        "$candidate/bin/$installed_name" \
        "$candidate/helpers/$update_helper" \
        "$candidate/helpers/$uninstall_helper" \
        "$candidate/config/packages.cfg" \
        "$candidate/config/install.cfg" \
        "$candidate/config/SHA256SUMS.common" \
        "$candidate/config/SHA256SUMS.$platform"; do
        [ -f "$asset" ] && [ ! -L "$asset" ] || return 1
    done
    case "$installed_name" in
        *.exe) ;;
        *)
            [ -x "$candidate/bin/$installed_name" ] &&
                [ -x "$candidate/helpers/$update_helper" ] || return 1
            ;;
    esac

    checksum_file_has_exact_entries "$candidate/config/SHA256SUMS.common" \
        packages.cfg install.cfg install.sh install.ps1 || return 1
    checksum_file_has_exact_entries "$candidate/config/SHA256SUMS.$platform" \
        "$cup_asset" "$uninstall_helper" release.txt || return 1
    legacy_checksum_matches "$candidate/config/SHA256SUMS.$platform" \
        "$cup_asset" "$candidate/bin/$installed_name" || return 1
    legacy_checksum_matches "$candidate/config/SHA256SUMS.$platform" \
        "$uninstall_helper" "$candidate/helpers/$uninstall_helper" || return 1
    legacy_checksum_matches "$candidate/config/SHA256SUMS.common" \
        packages.cfg "$candidate/config/packages.cfg" || return 1
    legacy_checksum_matches "$candidate/config/SHA256SUMS.common" \
        install.cfg "$candidate/config/install.cfg" || return 1
    file_sha256 "$candidate/bin/$installed_name" || return 1
    binary_hash=$FILE_SHA256
    file_sha256 "$candidate/helpers/$update_helper" || return 1
    [ "$binary_hash" = "$FILE_SHA256" ] || return 1
    legacy_catalog_is_valid "$candidate/config/packages.cfg" || return 1
    legacy_policy_is_valid "$candidate/config/install.cfg" || return 1
    legacy_state_is_valid "$candidate/state.txt"
}

root_candidate_status() {
    candidate=$1
    installed_name=$2
    platform=$3

    if [ ! -e "$candidate" ] && [ ! -L "$candidate" ]; then
        printf '%s\n' missing
    elif [ ! -d "$candidate" ] || [ -L "$candidate" ]; then
        printf '%s\n' foreign
    elif root_marker_is_valid "$candidate"; then
        printf '%s\n' owned
    elif [ -e "$candidate/root.txt" ] || [ -L "$candidate/root.txt" ]; then
        if root_has_cup_traces "$candidate" "$installed_name"; then
            printf '%s\n' invalid-marker
        else
            printf '%s\n' foreign
        fi
    elif legacy_root_is_valid "$candidate" "$installed_name" "$platform"; then
        printf '%s\n' legacy
    elif root_has_cup_binary "$candidate" "$installed_name"; then
        printf '%s\n' damaged
    else
        printf '%s\n' foreign
    fi
}

select_cup_root() {
    home=$1
    installed_name=$2
    platform=$3
    primary="$home/.cup"
    alternative="$home/.coffee-cup"
    primary_status="$(root_candidate_status "$primary" "$installed_name" "$platform")"
    alternative_status="$(root_candidate_status "$alternative" "$installed_name" "$platform")"

    case "$primary_status:$alternative_status" in
        damaged:* )
            fail "a probable legacy cup root was found but its installed generation could not be verified: $primary; the alternative root was preserved"
            ;;
        *:damaged )
            fail "a probable legacy cup root was found but its installed generation could not be verified: $alternative; the primary root was preserved"
            ;;
        invalid-marker:* )
            fail "cup root marker is invalid for the recognized root: $primary; the alternative root was preserved"
            ;;
        *:invalid-marker )
            fail "cup root marker is invalid for the recognized root: $alternative; the primary root was preserved"
            ;;
        owned:owned|owned:legacy|legacy:owned|legacy:legacy)
            fail "both cup root candidates are recognized: $primary and $alternative"
            ;;
        owned:*|legacy:*) SELECTED_CUP_ROOT=$primary ;;
        *:owned|*:legacy) SELECTED_CUP_ROOT=$alternative ;;
        missing:*) SELECTED_CUP_ROOT=$primary ;;
        foreign:missing) SELECTED_CUP_ROOT=$alternative ;;
        *) fail "neither existing cup root candidate is recognized: $primary or $alternative" ;;
    esac
}
ensure_root_marker() {
    if root_marker_is_valid "$CUP_ROOT"; then
        return
    fi
    if [ -e "$ROOT_MARKER" ] || [ -L "$ROOT_MARKER" ]; then
        fail "cup root marker is invalid: $ROOT_MARKER"
    fi
    marker_temp="$(mktemp "$CUP_ROOT/.root-marker.XXXXXX")" ||
        fail "could not create cup root marker"
    if ! printf 'format=1\nproduct=coffee-clang/cup\nlayout=1\n' > "$marker_temp" ||
        ! chmod 0600 "$marker_temp" ||
        ! mv "$marker_temp" "$ROOT_MARKER"; then
        rm -f "$marker_temp"
        fail "could not install cup root marker"
    fi
}

# Selected root and bootstrap paths.
configure_paths() {
    CUP_ROOT="$1"
    uninstall_name="$2"
    platform="$3"
    CUP_BIN_DIR="$CUP_ROOT/bin"
    CUP_CONFIG_DIR="$CUP_ROOT/config"
    CUP_HELPERS_DIR="$CUP_ROOT/helpers"
    PACKAGES_CFG="$CUP_CONFIG_DIR/packages.cfg"
    INSTALL_CONFIG="$CUP_CONFIG_DIR/install.cfg"
    COMMON_CHECKSUMS="$CUP_CONFIG_DIR/SHA256SUMS.common"
    PLATFORM_CHECKSUMS="$CUP_CONFIG_DIR/SHA256SUMS.$platform"
    UNINSTALL_SCRIPT="$CUP_HELPERS_DIR/$uninstall_name"
    case "$platform" in
        windows-*)
            UPDATE_HELPER="$CUP_HELPERS_DIR/cup-update-helper.exe"
            ;;
        *)
            UPDATE_HELPER="$CUP_HELPERS_DIR/cup-update-helper"
            ;;
    esac
    ROOT_MARKER="$CUP_ROOT/root.txt"
}

# Download and checksum validation.
download_file() {
    url="$1"
    output="$2"
    if command -v curl >/dev/null 2>&1; then
        case "$BASE_URL" in
            https://*)
                curl -fsSL --proto '=https' --proto-redir '=https' \
                    "$url" -o "$output" || fail "failed to download $url"
                ;;
            *)
                curl -fsSL "$url" -o "$output" || fail "failed to download $url"
                ;;
        esac
    elif command -v wget >/dev/null 2>&1; then
        case "$BASE_URL" in
            https://*)
                wget -q --https-only "$url" -O "$output" || fail "failed to download $url"
                ;;
            *)
                wget -q "$url" -O "$output" || fail "failed to download $url"
                ;;
        esac
    else
        fail "neither curl nor wget is available"
    fi
    [ -s "$output" ] || fail "downloaded file is empty: $url"
}

assert_checksum_entries() {
    checksum_file="$1"
    shift
    [ -s "$checksum_file" ] || fail "checksum file is empty"

    entry_count=0
    checksum_line=
    while IFS= read -r checksum_line || [ -n "$checksum_line" ]; do
        [ -n "$checksum_line" ] && parse_checksum_line "$checksum_line" ||
            fail "checksum file contains invalid or unexpected entries"
        entry_count=$((entry_count + 1))
    done < "$checksum_file"
    [ "$entry_count" -eq "$#" ] ||
        fail "checksum file contains invalid or unexpected entries"

    for expected in "$@"; do
        matches=0
        checksum_line=
        while IFS= read -r checksum_line || [ -n "$checksum_line" ]; do
            parse_checksum_line "$checksum_line" ||
                fail "checksum file contains invalid or unexpected entries"
            if [ "$CHECKSUM_NAME" = "$expected" ]; then
                matches=$((matches + 1))
            fi
        done < "$checksum_file"
        [ "$matches" -eq 1 ] ||
            fail "checksum entry is missing or duplicated: $expected"
    done
}

verify_checksum_file() {
    directory="$1"
    checksum_file="$2"
    shift 2
    assert_checksum_entries "$directory/$checksum_file" "$@"

    if command -v sha256sum >/dev/null 2>&1; then
        (cd "$directory" && sha256sum -c "$checksum_file") >/dev/null ||
            fail "checksum verification failed"
    elif command -v shasum >/dev/null 2>&1; then
        (cd "$directory" && shasum -a 256 -c "$checksum_file") >/dev/null ||
            fail "checksum verification failed"
    else
        fail "neither sha256sum nor shasum is available"
    fi
}

validate_release_metadata() {
    metadata="$1"
    expected_version="${2:-}"
    expected_commit="${3:-}"

    [ -f "$metadata" ] || fail "release metadata file is missing: $metadata"
    [ -r "$metadata" ] || fail "release metadata file is not readable: $metadata"

    seen_format=0
    seen_version=0
    seen_commit=0
    metadata_format=
    metadata_version=
    metadata_commit=
    line_number=0
    carriage_return=$(printf '\r')
    metadata_line=

    while IFS= read -r metadata_line || [ -n "$metadata_line" ]; do
        line_number=$((line_number + 1))
        case "$metadata_line" in
            *"$carriage_return") metadata_line=${metadata_line%?} ;;
        esac
        case "$metadata_line" in
            *=*)
                key=${metadata_line%%=*}
                value=${metadata_line#*=}
                ;;
            *)
                fail "release metadata line $line_number must contain exactly one non-empty 'key=value' assignment: $metadata"
                ;;
        esac
        [ -n "$key" ] && [ -n "$value" ] && [ "${value#*=}" = "$value" ] ||
            fail "release metadata line $line_number must contain exactly one non-empty 'key=value' assignment: $metadata"

        case "$key" in
            format)
                [ "$seen_format" -eq 0 ] ||
                    fail "release metadata field 'format' is duplicated: $metadata"
                seen_format=1
                metadata_format=$value
                ;;
            version)
                [ "$seen_version" -eq 0 ] ||
                    fail "release metadata field 'version' is duplicated: $metadata"
                seen_version=1
                metadata_version=$value
                ;;
            commit)
                [ "$seen_commit" -eq 0 ] ||
                    fail "release metadata field 'commit' is duplicated: $metadata"
                seen_commit=1
                metadata_commit=$value
                ;;
            *)
                fail "release metadata contains an unexpected field at line $line_number: $metadata"
                ;;
        esac
    done < "$metadata"

    [ "$seen_format" -eq 1 ] ||
        fail "release metadata is missing required field 'format': $metadata"
    [ "$seen_version" -eq 1 ] ||
        fail "release metadata is missing required field 'version': $metadata"
    [ "$seen_commit" -eq 1 ] ||
        fail "release metadata is missing required field 'commit': $metadata"
    [ "$line_number" -eq 3 ] ||
        fail "release metadata must contain exactly 3 lines; found $line_number: $metadata"
    [ "$metadata_format" = 1 ] ||
        fail "release metadata format is unsupported; expected '1': $metadata"
    is_release_version "$metadata_version" ||
        fail "release metadata version is invalid; expected 'MAJOR.MINOR.PATCH': $metadata"
    is_lower_hex_range "$metadata_commit" 7 40 ||
        fail "release metadata commit is invalid; expected 7 to 40 lowercase hexadecimal characters: $metadata"
    if [ -n "$expected_version" ] && [ "$metadata_version" != "$expected_version" ]; then
        fail "release metadata version mismatch: expected '$expected_version', received '$metadata_version': $metadata"
    fi
    if [ -n "$expected_commit" ] && [ "$metadata_commit" != "$expected_commit" ]; then
        fail "release metadata commit mismatch: expected '$expected_commit', received '$metadata_commit': $metadata"
    fi
}

verify_named_checksum() {
    directory="$1"
    checksum_file="$2"
    expected="$3"
    selected="$directory/.cup-selected-checksum"

    matches=0
    selected_line=
    checksum_line=
    while IFS= read -r checksum_line || [ -n "$checksum_line" ]; do
        parse_checksum_line "$checksum_line" ||
            fail "checksum file contains invalid entries: $checksum_file"
        if [ "$CHECKSUM_NAME" = "$expected" ]; then
            matches=$((matches + 1))
            selected_line=$checksum_line
        fi
    done < "$checksum_file"
    [ "$matches" -eq 1 ] ||
        fail "checksum entry is missing or duplicated: $expected"
    printf '%s\n' "$selected_line" > "$selected"
    if command -v sha256sum >/dev/null 2>&1; then
        (cd "$directory" && sha256sum -c "${selected##*/}") >/dev/null ||
            fail "checksum verification failed for $expected"
    elif command -v shasum >/dev/null 2>&1; then
        (cd "$directory" && shasum -a 256 -c "${selected##*/}") >/dev/null ||
            fail "checksum verification failed for $expected"
    else
        fail "neither sha256sum nor shasum is available"
    fi
    rm -f "$selected"
}

validate_profile_path() {
    profile_path="$1"
    profile_parent="${profile_path%/*}"
    [ "$profile_parent" != "$profile_path" ] || profile_parent="."

    [ ! -L "$profile_path" ] ||
        fail "shell profile is a symbolic link and was not modified: $profile_path"
    if [ -e "$profile_path" ] && [ ! -f "$profile_path" ]; then
        fail "shell profile is not a regular file and was not modified: $profile_path"
    fi
    case "$profile_path" in
        "$HOME"/*) ;;
        *)
            fail "shell profile is outside HOME and was not modified: $profile_path"
            ;;
    esac
    current_parent="$profile_parent"
    while [ "$current_parent" != "$HOME" ]; do
        [ ! -L "$current_parent" ] ||
            fail "shell profile directory is a symbolic link and was not modified: $current_parent"
        if [ -e "$current_parent" ] && [ ! -d "$current_parent" ]; then
            fail "shell profile parent is not a directory: $current_parent"
        fi
        next_parent="${current_parent%/*}"
        [ -n "$next_parent" ] || next_parent="/"
        [ "$next_parent" != "$current_parent" ] ||
            fail "shell profile parent could not be validated: $profile_parent"
        current_parent="$next_parent"
    done
}

append_profile_line() {
    profile_path="$1"
    line="$2"
    profile_parent="${profile_path%/*}"
    [ "$profile_parent" != "$profile_path" ] || profile_parent="."

    validate_profile_path "$profile_path"
    mkdir -p "$profile_parent"
    validate_profile_path "$profile_path"
    need_command mktemp
    profile_temp="$(mktemp "$profile_parent/.cup-profile.XXXXXX")" ||
        fail "could not create a private shell-profile temporary file"
    if [ -f "$profile_path" ]; then
        cp -p "$profile_path" "$profile_temp" || {
            rm -f "$profile_temp"
            fail "could not copy shell profile safely"
        }
        printf '\n%s\n' "$line" >> "$profile_temp" || {
            rm -f "$profile_temp"
            fail "could not prepare shell profile update"
        }
    else
        chmod 0600 "$profile_temp"
        printf '%s\n' "$line" > "$profile_temp" || {
            rm -f "$profile_temp"
            fail "could not prepare shell profile update"
        }
    fi
    validate_profile_path "$profile_path"
    mv "$profile_temp" "$profile_path" || {
        rm -f "$profile_temp"
        fail "could not replace shell profile atomically"
    }
}

# Optional user PATH integration.
detect_shell_profile() {
    shell_path=${SHELL:-}
    shell_name=${shell_path##*/}
    case "$shell_name" in
        fish)
            printf '%s\n' "$HOME/.config/fish/conf.d/cup.fish"
            ;;
        zsh)
            printf '%s\n' "$HOME/.zshrc"
            ;;
        bash)
            printf '%s\n' "$HOME/.bashrc"
            ;;
        *)
            if [ -f "$HOME/.bashrc" ]; then
                printf '%s\n' "$HOME/.bashrc"
            else
                printf '%s\n' "$HOME/.profile"
            fi
            ;;
    esac
}

shell_quote() {
    quote_value=$1
    printf "'"
    while [ -n "$quote_value" ]; do
        quote_character=${quote_value%"${quote_value#?}"}
        quote_value=${quote_value#?}
        case "$quote_character" in
            "'") printf "'\\''" ;;
            *) printf '%s' "$quote_character" ;;
        esac
    done
    printf "'"
}

cup_bin_in_current_path() {
    remaining_path=${PATH:-}
    while :; do
        case "$remaining_path" in
            *:*)
                path_entry=${remaining_path%%:*}
                remaining_path=${remaining_path#*:}
                ;;
            *)
                path_entry=$remaining_path
                remaining_path=
                ;;
        esac
        [ "$path_entry" = "$CUP_BIN_DIR" ] && return 0
        [ -n "$remaining_path" ] || break
    done
    return 1
}

offer_path_update() {
    shell_path=${SHELL:-}
    shell_name=${shell_path##*/}
    profile="$(detect_shell_profile)"
    if [ "$shell_name" = fish ]; then
        quoted_bin="$(shell_quote "$CUP_BIN_DIR")"
        path_line="fish_add_path $quoted_bin"
    else
        quoted_bin="$(shell_quote "$CUP_BIN_DIR")"
        path_line="export PATH=$quoted_bin:\"\$PATH\""
    fi
    if cup_bin_in_current_path; then
        CUP_AVAILABLE_IN_PATH=1
        info "cup bin directory is already available in PATH for this shell."
        return
    fi
    if [ -f "$profile" ] && file_contains_line "$path_line" "$profile"; then
        CUP_AVAILABLE_IN_PATH=1
        info "PATH entry already exists in $profile."
        info "Restart the shell or run:"
        info "  $path_line"
        return
    fi
    if [ "${CUP_INSTALL_NO_PATH_PROMPT:-0}" = 1 ]; then
        info "PATH not modified. Add this line manually when needed:"
        info "  $path_line"
        return
    fi
    answer="$(prompt_tty "Add $CUP_BIN_DIR to PATH in $profile? [y/N] " "")"
    case "$answer" in
        y|Y|yes|YES)
            append_profile_line "$profile" "$path_line"
            CUP_AVAILABLE_IN_PATH=1
            info "PATH updated in $profile. Restart the shell or run:"
            info "  $path_line"
            ;;
        *)
            info "PATH not modified. Add this line manually when needed:"
            info "  $path_line"
            ;;
    esac
}

# Recoverable bootstrap replacement.
assert_real_directory() {
    path="$1"
    if [ -L "$path" ]; then
        fail "managed directory is a symbolic link: $path"
    fi
    if [ -e "$path" ] && [ ! -d "$path" ]; then
        fail "managed path is not a directory: $path"
    fi
}

set_windows_read_only() {
    need_command cygpath
    need_command attrib.exe
    attrib.exe +R "$(cygpath -w "$1")" >/dev/null ||
        fail "failed to set read-only attribute on $1"
}

clear_read_only() {
    path="$1"
    [ -e "$path" ] || [ -L "$path" ] || return 0
    [ -L "$path" ] && return 0
    if [ "$WINDOWS_SHELL_INSTALL" -eq 1 ]; then
        attrib.exe -R "$(cygpath -w "$path")" >/dev/null
    else
        chmod u+w "$path"
    fi
}

is_regular_asset() {
    [ -f "$1" ] && [ ! -L "$1" ]
}

restore_permissions() {
    require_assets="${1:-0}"
    if [ "$require_assets" -eq 1 ]; then
        for path in "$PACKAGES_CFG" "$INSTALL_CONFIG" "$COMMON_CHECKSUMS" \
            "$PLATFORM_CHECKSUMS" "$UNINSTALL_SCRIPT" "$UPDATE_HELPER"; do
            is_regular_asset "$path" || return 1
        done
    fi

    if is_regular_asset "$PACKAGES_CFG"; then chmod 0444 "$PACKAGES_CFG" || return 1; fi
    if is_regular_asset "$INSTALL_CONFIG"; then chmod 0444 "$INSTALL_CONFIG" || return 1; fi
    if is_regular_asset "$COMMON_CHECKSUMS"; then chmod 0444 "$COMMON_CHECKSUMS" || return 1; fi
    if is_regular_asset "$PLATFORM_CHECKSUMS"; then chmod 0444 "$PLATFORM_CHECKSUMS" || return 1; fi
    if [ "$WINDOWS_SHELL_INSTALL" -eq 1 ]; then
        if is_regular_asset "$PACKAGES_CFG"; then set_windows_read_only "$PACKAGES_CFG" || return 1; fi
        if is_regular_asset "$INSTALL_CONFIG"; then set_windows_read_only "$INSTALL_CONFIG" || return 1; fi
        if is_regular_asset "$COMMON_CHECKSUMS"; then set_windows_read_only "$COMMON_CHECKSUMS" || return 1; fi
        if is_regular_asset "$PLATFORM_CHECKSUMS"; then set_windows_read_only "$PLATFORM_CHECKSUMS" || return 1; fi
        if is_regular_asset "$UNINSTALL_SCRIPT"; then set_windows_read_only "$UNINSTALL_SCRIPT" || return 1; fi
    elif is_regular_asset "$UNINSTALL_SCRIPT"; then
        chmod 0555 "$UNINSTALL_SCRIPT" || return 1
    fi
    if is_regular_asset "$UPDATE_HELPER"; then chmod 0755 "$UPDATE_HELPER" || return 1; fi
    return 0
}

rollback_asset() {
    key="$1"
    destination="$2"
    staging="$3"
    backup="$staging/backup/$key"
    absent="$backup.absent"
    installed="$staging/installed/$key"

    clear_read_only "$destination" || return 1
    if { [ -e "$destination" ] || [ -L "$destination" ]; } &&
        ! is_regular_asset "$destination"; then
        return 1
    fi
    for evidence in "$backup" "$absent" "$installed"; do
        if { [ -e "$evidence" ] || [ -L "$evidence" ]; } &&
            ! is_regular_asset "$evidence"; then
            return 1
        fi
    done
    if is_regular_asset "$backup" && is_regular_asset "$absent"; then
        return 1
    fi
    if is_regular_asset "$installed" &&
        ! is_regular_asset "$backup" && ! is_regular_asset "$absent"; then
        return 1
    fi

    if is_regular_asset "$backup"; then
        mv -f "$backup" "$destination" || return 1
    elif is_regular_asset "$absent"; then
        if is_regular_asset "$installed"; then
            rm -f "$destination" || return 1
        elif [ -e "$destination" ] || [ -L "$destination" ]; then
            return 1
        fi
    fi
    return 0
}

recover_staging() {
    staging="$1"
    cup_bin="$2"
    if [ -L "$staging" ]; then
        fail "bootstrap staging path is a symbolic link: $staging"
    fi
    [ -d "$staging" ] || return 0

    if [ -f "$staging/committed" ] && [ ! -L "$staging/committed" ]; then
        is_regular_asset "$cup_bin" && restore_permissions 1 ||
            fail "completed bootstrap staging does not match a complete installed generation"
        info "Finishing cleanup from a completed cup bootstrap installation."
        rm -rf "$staging" ||
            fail "could not remove completed bootstrap staging directory"
        return 0
    fi
    if [ -e "$staging/committed" ] || [ -L "$staging/committed" ]; then
        fail "bootstrap commit marker is not a regular file: $staging/committed"
    fi

    info "Recovering an interrupted cup bootstrap installation."
    recovery_failed=0
    rollback_asset catalog "$PACKAGES_CFG" "$staging" || recovery_failed=1
    rollback_asset install-config "$INSTALL_CONFIG" "$staging" || recovery_failed=1
    rollback_asset common-checksums "$COMMON_CHECKSUMS" "$staging" || recovery_failed=1
    rollback_asset platform-checksums "$PLATFORM_CHECKSUMS" "$staging" || recovery_failed=1
    rollback_asset uninstall "$UNINSTALL_SCRIPT" "$staging" || recovery_failed=1
    rollback_asset update-helper "$UPDATE_HELPER" "$staging" || recovery_failed=1
    rollback_asset binary "$cup_bin" "$staging" || recovery_failed=1
    restore_permissions || recovery_failed=1

    if [ "$recovery_failed" -ne 0 ]; then
        fail "the previous bootstrap installation could not be recovered; \
staging was preserved at $staging"
    fi
    rm -rf "$staging" || fail "could not remove recovered staging directory"
}

backup_asset() {
    key="$1"
    destination="$2"
    staging="$3"
    clear_read_only "$destination"
    if is_regular_asset "$destination"; then
        cp -p "$destination" "$staging/backup/$key" ||
            fail "could not back up $destination"
    elif [ -e "$destination" ] || [ -L "$destination" ]; then
        fail "existing bootstrap asset is not a regular file: $destination"
    else
        : > "$staging/backup/$key.absent" ||
            fail "could not record absent asset: $destination"
    fi
}

commit_asset() {
    key="$1"
    source="$2"
    destination="$3"
    staging="$4"

    is_regular_asset "$source" ||
        fail "bootstrap staged asset is not a regular file: $source"
    if { [ -e "$destination" ] || [ -L "$destination" ]; } &&
        ! is_regular_asset "$destination"; then
        fail "installed bootstrap asset is not a regular file: $destination"
    fi
    : > "$staging/installed/$key" ||
        fail "could not record bootstrap replacement: $destination"
    mv -f "$source" "$destination" ||
        fail "could not install bootstrap asset: $destination"
}

cleanup_uninstall_residues() {
    installed_name=$1

    for residue in "$HOME"/.cup-uninstall.*; do
        [ -e "$residue" ] || [ -L "$residue" ] || continue
        if [ -L "$residue" ] || [ ! -d "$residue" ]; then
            fail "unrecognized uninstall residue was preserved: $residue"
        fi
        residue_name=${residue##*/}
        case "$residue_name" in
            .cup-uninstall.*) token=${residue_name#.cup-uninstall.} ;;
            *) token= ;;
        esac
        case "$token" in
            ''|*[!a-zA-Z0-9_-]*)
                fail "unrecognized uninstall residue was preserved: $residue"
                ;;
        esac
        journal="$residue/transaction.txt"
        if ! root_marker_is_valid "$residue" ||
            [ ! -f "$residue/bin/$installed_name" ] ||
            [ -L "$residue/bin/$installed_name" ] ||
            [ ! -f "$journal" ] || [ -L "$journal" ] ||
            ! read_exact_seven_lines "$journal" ||
            [ "$FILE_LINE_1" != "format=1" ] ||
            [ "$FILE_LINE_2" != "operation=uninstall" ] ||
            [ "$FILE_LINE_4" != "temporary_name=$residue_name" ] ||
            [ "$FILE_LINE_5" != "token=$token" ]; then
            fail "unrecognized uninstall residue was preserved: $residue"
        fi
        case "$FILE_LINE_3:$FILE_LINE_6:$FILE_LINE_7" in
            phase=detaching:stage=detach:error=0) ;;
            phase=failed:stage=cleanup:error=*)
                residue_error=${FILE_LINE_7#error=}
                case "$residue_error" in
                    ''|*[!0-9]*|0)
                        fail "unrecognized uninstall residue was preserved: $residue"
                        ;;
                esac
                ;;
            *)
                fail "unrecognized uninstall residue was preserved: $residue"
                ;;
        esac
        info "Removing validated uninstall residue: $residue"
        rm -rf "$residue" ||
            fail "could not remove validated uninstall residue: $residue"
    done
}


# Bootstrap transfer and transaction phases are kept separate so the main
# installer reads as an ordered recovery-safe pipeline.
download_bootstrap_assets() (
    staging=$1
    cup_asset=$2
    platform=$3
    uninstall_asset=$4

    download_file "$BASE_URL/$cup_asset" "$staging/$cup_asset"
    download_file "$BASE_URL/packages.cfg" "$staging/packages.cfg"
    download_file "$BASE_URL/install.cfg" "$staging/install.cfg"
    download_file "$BASE_URL/$uninstall_asset" "$staging/$uninstall_asset"
    download_file "$BASE_URL/release.txt" "$staging/release.txt"
    download_file "$BASE_URL/SHA256SUMS.$platform" "$staging/SHA256SUMS.$platform"
    download_file "$BASE_URL/SHA256SUMS.common" "$staging/SHA256SUMS.common"
)

verify_bootstrap_assets() (
    staging=$1
    cup_asset=$2
    platform=$3
    uninstall_asset=$4

    verify_checksum_file "$staging" "SHA256SUMS.$platform" \
        "$cup_asset" "$uninstall_asset" "release.txt"
    assert_checksum_entries "$staging/SHA256SUMS.common" \
        "packages.cfg" "install.cfg" "install.sh" "install.ps1"
    verify_named_checksum "$staging" "$staging/SHA256SUMS.common" "packages.cfg"
    verify_named_checksum "$staging" "$staging/SHA256SUMS.common" "install.cfg"
    validate_release_metadata \
        "$staging/release.txt" "$CUP_RELEASE_VERSION" "$CUP_RELEASE_COMMIT"
    cp "$staging/$cup_asset" "$staging/cup-update-helper"
    chmod 0755 "$staging/$cup_asset" "$staging/cup-update-helper"
    if [ "$WINDOWS_SHELL_INSTALL" -eq 0 ]; then
        chmod 0555 "$staging/$uninstall_asset"
    fi
)

backup_bootstrap_assets() (
    staging=$1
    cup_bin=$2

    backup_asset catalog "$PACKAGES_CFG" "$staging"
    backup_asset install-config "$INSTALL_CONFIG" "$staging"
    backup_asset common-checksums "$COMMON_CHECKSUMS" "$staging"
    backup_asset platform-checksums "$PLATFORM_CHECKSUMS" "$staging"
    backup_asset uninstall "$UNINSTALL_SCRIPT" "$staging"
    backup_asset update-helper "$UPDATE_HELPER" "$staging"
    backup_asset binary "$cup_bin" "$staging"
)

commit_bootstrap_assets() (
    staging=$1
    cup_asset=$2
    cup_bin=$3
    platform=$4
    uninstall_asset=$5

    commit_asset catalog "$staging/packages.cfg" "$PACKAGES_CFG" "$staging"
    commit_asset install-config "$staging/install.cfg" "$INSTALL_CONFIG" "$staging"
    commit_asset common-checksums \
        "$staging/SHA256SUMS.common" "$COMMON_CHECKSUMS" "$staging"
    commit_asset platform-checksums \
        "$staging/SHA256SUMS.$platform" "$PLATFORM_CHECKSUMS" "$staging"
    commit_asset uninstall "$staging/$uninstall_asset" "$UNINSTALL_SCRIPT" "$staging"
    commit_asset update-helper "$staging/cup-update-helper" "$UPDATE_HELPER" "$staging"
    commit_asset binary "$staging/$cup_asset" "$cup_bin" "$staging"
)

install_assets() {
    cup_asset="$1"
    installed_name="$2"
    platform="$3"
    uninstall_asset="$4"
    cup_bin="$CUP_BIN_DIR/$installed_name"
    staging="$CUP_ROOT/.bootstrap"

    cleanup_uninstall_residues "$installed_name"
    assert_real_directory "$CUP_ROOT"
    assert_real_directory "$CUP_BIN_DIR"
    assert_real_directory "$CUP_CONFIG_DIR"
    assert_real_directory "$CUP_HELPERS_DIR"
    mkdir -p "$CUP_BIN_DIR" "$CUP_CONFIG_DIR" "$CUP_HELPERS_DIR"
    chmod 0700 "$CUP_ROOT" "$CUP_BIN_DIR" "$CUP_CONFIG_DIR" "$CUP_HELPERS_DIR" ||
        fail "could not make the cup root private"
    ensure_root_marker
    if [ "$WINDOWS_SHELL_INSTALL" -eq 1 ]; then
        need_command cygpath
        need_command attrib.exe
    fi
    recover_staging "$staging" "$cup_bin"
    mkdir "$staging"
    mkdir "$staging/backup" "$staging/installed"
    committed=0

    rollback() {
        if [ "$committed" -eq 0 ] && [ -d "$staging" ]; then
            recovery_failed=0
            rollback_asset catalog "$PACKAGES_CFG" "$staging" || recovery_failed=1
            rollback_asset install-config "$INSTALL_CONFIG" "$staging" || recovery_failed=1
            rollback_asset common-checksums "$COMMON_CHECKSUMS" "$staging" || recovery_failed=1
            rollback_asset platform-checksums "$PLATFORM_CHECKSUMS" "$staging" || recovery_failed=1
            rollback_asset uninstall "$UNINSTALL_SCRIPT" "$staging" || recovery_failed=1
            rollback_asset update-helper "$UPDATE_HELPER" "$staging" || recovery_failed=1
            rollback_asset binary "$cup_bin" "$staging" || recovery_failed=1
            restore_permissions || recovery_failed=1
            if [ "$recovery_failed" -eq 0 ]; then
                rm -rf "$staging"
            else
                printf '%s\n' \
                    "Error: rollback was incomplete; staging was preserved at $staging" >&2
            fi
        fi
    }
    trap rollback 0 HUP INT TERM

    info "Installing cup into $CUP_ROOT"
    download_bootstrap_assets "$staging" "$cup_asset" "$platform" "$uninstall_asset"
    verify_bootstrap_assets "$staging" "$cup_asset" "$platform" "$uninstall_asset"
    backup_bootstrap_assets "$staging" "$cup_bin"
    commit_bootstrap_assets \
        "$staging" "$cup_asset" "$cup_bin" "$platform" "$uninstall_asset"

    chmod 0755 "$cup_bin"
    restore_permissions 1 || fail "installed bootstrap assets are incomplete or unsafe"
    : > "$staging/committed" ||
        fail "could not record completed bootstrap installation"
    committed=1
    trap - 0 HUP INT TERM
    if ! rm -rf "$staging"; then
        printf '%s\n' \
            "Warning: cup was installed, but completed bootstrap staging could not be removed." \
            >&2
    fi

    info "cup installed successfully."
    info "Binary:    $cup_bin"
    info "Package catalog: $PACKAGES_CFG"
    info "Install configuration: $INSTALL_CONFIG"
    info "Checksums: $COMMON_CHECKSUMS"
    info "           $PLATFORM_CHECKSUMS"
    info "Uninstall: $UNINSTALL_SCRIPT"
    offer_path_update
    if [ "$CUP_AVAILABLE_IN_PATH" -eq 1 ]; then
        info "Test with: cup help"
    else
        info "Test with: $cup_bin help"
    fi
}

# Native POSIX installation.
install_unix() {
    [ -n "${HOME:-}" ] || fail "HOME is not set"
    case "$HOME" in
        /)
            fail "HOME must not be the filesystem root"
            ;;
        /*) ;;
        *)
            fail "HOME must contain an absolute path"
            ;;
    esac

    require_shell_install_commands
    os="$(uname -s 2>/dev/null || true)"
    arch="$(uname -m 2>/dev/null || true)"
    case "$os:$arch" in
        Linux:x86_64|Linux:amd64)
            asset=cup-linux-x64
            platform=linux-x64
            ;;
        Linux:aarch64|Linux:arm64)
            asset=cup-linux-arm64
            platform=linux-arm64
            ;;
        Darwin:x86_64|Darwin:amd64)
            asset=cup-macos-x64
            platform=macos-x64
            ;;
        Darwin:arm64|Darwin:aarch64)
            asset=cup-macos-arm64
            platform=macos-arm64
            ;;
        *)
            fail "unsupported platform: $os $arch"
            ;;
    esac
    WINDOWS_SHELL_INSTALL=0
    select_cup_root "$HOME" "cup" "$platform"
    configure_paths "$SELECTED_CUP_ROOT" "uninstall.sh" "$platform"
    install_assets "$asset" "cup" "$platform" "uninstall.sh"
}

# Windows shell delegation and native fallback.
run_powershell_installer() {
    need_command mktemp
    delegate_dir="$(mktemp -d "${TMPDIR:-/tmp}/cup-install.XXXXXX")" ||
        fail "could not create a private installer directory"
    delegate_cleanup() {
        rm -rf "$delegate_dir"
    }
    trap delegate_cleanup 0 HUP INT TERM

    download_file "$BASE_URL/SHA256SUMS.common" "$delegate_dir/SHA256SUMS.common"
    download_file "$BASE_URL/install.ps1" "$delegate_dir/install.ps1"
    assert_checksum_entries "$delegate_dir/SHA256SUMS.common" \
        "packages.cfg" "install.cfg" "install.sh" "install.ps1"
    verify_named_checksum "$delegate_dir" "$delegate_dir/SHA256SUMS.common" "install.ps1"

    need_command cygpath
    installer_windows="$(cygpath -w "$delegate_dir/install.ps1")"
    if command -v powershell.exe >/dev/null 2>&1; then
        if powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$installer_windows"; then
            status=0
        else
            status=$?
        fi
    elif command -v pwsh.exe >/dev/null 2>&1; then
        if pwsh.exe -NoProfile -File "$installer_windows"; then
            status=0
        else
            status=$?
        fi
    else
        fail "PowerShell was not found. Run the verified Windows installer manually."
    fi
    trap - 0 HUP INT TERM
    delegate_cleanup
    exit "$status"
}

get_windows_profile_root() {
    windows_profile="${USERPROFILE:-}"
    need_command cygpath
    if [ -z "$windows_profile" ] && command -v cmd.exe >/dev/null 2>&1; then
        windows_profile="$(cmd.exe /d /c echo %USERPROFILE% 2>/dev/null)"
        carriage_return=$(printf '\r')
        case "$windows_profile" in
            *"$carriage_return") windows_profile=${windows_profile%?} ;;
        esac
    fi
    [ -n "$windows_profile" ] || fail "Windows user profile could not be determined"
    cygpath -u "$windows_profile"
}

install_windows_from_shell_directly() {
    require_shell_install_commands
    arch="$(uname -m 2>/dev/null || true)"
    case "$arch" in
        x86_64|amd64) ;;
        *)
            fail "unsupported Windows architecture: $arch. This installer supports x64 only"
            ;;
    esac
    WINDOWS_SHELL_INSTALL=1
    windows_profile="$(get_windows_profile_root)"
    select_cup_root "$windows_profile" "cup.exe" "windows-x64"
    configure_paths "$SELECTED_CUP_ROOT" "uninstall.ps1" "windows-x64"
    install_assets "cup-windows-x64.exe" "cup.exe" "windows-x64" "uninstall.ps1"
}

install_windows_from_shell() {
    info "Windows Unix-like shell detected."
    info ""
    info "Choose installation mode:"
    info "  1) Native Windows installation via PowerShell"
    info "  2) Installation from the current shell"
    choice="$(prompt_tty "Choice [1/2, default: 1]: " "1")"
    case "$choice" in
        ""|1)
            run_powershell_installer
            ;;
        2)
            install_windows_from_shell_directly
            ;;
        *)
            fail "invalid choice"
            ;;
    esac
}

# Platform dispatch.
main() {
    validate_installer_identity
    validate_base_url
    need_command uname
    os="$(uname -s 2>/dev/null || true)"
    case "$os" in
        Linux|Darwin)
            install_unix
            ;;
        MINGW*|MSYS*|CYGWIN*)
            install_windows_from_shell
            ;;
        *)
            fail "unsupported operating system: $os"
            ;;
    esac
}

main "$@"
