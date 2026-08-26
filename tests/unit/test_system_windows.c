/*
 * Exercises the native Windows system contract without duplicating
 * command-level integration workflows.
 */

#include "error.h"
#include "path.h"
#include "system.h"
#include "unity.h"
#include "test_platform.h"
#include "windows_utf.h"

#include <errno.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <aclapi.h>
#include <accctrl.h>

static char temp_dir[CUP_TEST_TEMP_PATH_SIZE];
static char original_profile[CUP_TEST_TEMP_PATH_SIZE];
static int had_profile;

void setUp(void) {
}

void tearDown(void) {
}

static void build_path(char *out, size_t size, const char *name) {
    int written = snprintf(out, size, "%s/%s", temp_dir, name);

    TEST_ASSERT_TRUE(written >= 0 && (size_t)written < size);
}

static void write_text(const char *path, const char *text) {
    FILE *file = fopen(path, "wb");

    TEST_ASSERT_NOT_NULL(file);
    TEST_ASSERT_EQUAL_size_t(strlen(text), fwrite(text, 1, strlen(text), file));
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
}

static void read_text(const char *path, char *buffer, size_t size) {
    FILE *file = fopen(path, "rb");
    size_t count;

    TEST_ASSERT_NOT_NULL(file);
    count = fread(buffer, 1, size - 1, file);
    TEST_ASSERT_FALSE(ferror(file));
    buffer[count] = '\0';
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
}

static CupError count_entry(const char *path,
                            SystemPathKind kind,
                            const SystemPathIdentity *identity,
                            void *userdata) {
    size_t *count = userdata;

    TEST_ASSERT_NOT_NULL(path);
    TEST_ASSERT_NOT_NULL(identity);
    TEST_ASSERT_TRUE(identity->valid);
    TEST_ASSERT_EQUAL_INT(kind, identity->kind);
    TEST_ASSERT_TRUE(kind != SYSTEM_PATH_MISSING);
    (*count)++;
    return CUP_OK;
}

static CupError reject_entry(const char *path,
                             SystemPathKind kind,
                             const SystemPathIdentity *identity,
                             void *userdata) {
    (void)identity;
    (void)path;
    (void)kind;
    (void)userdata;
    return CUP_ERR_INTERRUPT;
}

static int always_cancel(void) {
    return 1;
}

static void assert_private_acl_is_inheritable(const char *path) {
    wchar_t wide_path[CUP_TEST_TEMP_PATH_SIZE];
    PSECURITY_DESCRIPTOR descriptor = NULL;
    PACL dacl = NULL;
    DWORD status;
    DWORD i;
    size_t allowed_count = 0;

    TEST_ASSERT_TRUE(MultiByteToWideChar(CP_UTF8,
                                         MB_ERR_INVALID_CHARS,
                                         path,
                                         -1,
                                         wide_path,
                                         CUP_TEST_TEMP_PATH_SIZE) > 0);
    status = GetNamedSecurityInfoW(wide_path,
                                   SE_FILE_OBJECT,
                                   DACL_SECURITY_INFORMATION,
                                   NULL,
                                   NULL,
                                   &dacl,
                                   NULL,
                                   &descriptor);
    TEST_ASSERT_EQUAL_UINT32(ERROR_SUCCESS, status);
    TEST_ASSERT_NOT_NULL(dacl);

    for (i = 0; i < dacl->AceCount; ++i) {
        ACE_HEADER *header = NULL;

        TEST_ASSERT_TRUE(GetAce(dacl, i, (void **)&header));
        if (header->AceType != ACCESS_ALLOWED_ACE_TYPE) {
            continue;
        }
        TEST_ASSERT_EQUAL_HEX8(OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE,
                               header->AceFlags &
                                   (OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE));
        allowed_count++;
    }

    TEST_ASSERT_EQUAL_size_t(3, allowed_count);
    LocalFree(descriptor);
}

static int wait_for_path(const char *path) {
    int attempt;

    for (attempt = 0; attempt < 100; ++attempt) {
        DWORD attributes = GetFileAttributesA(path);

        if (attributes != INVALID_FILE_ATTRIBUTES) {
            return 1;
        }
        Sleep(50);
    }
    return 0;
}

static int wait_for_path_missing(const char *path) {
    int attempt;

    for (attempt = 0; attempt < 100; ++attempt) {
        if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES &&
            GetLastError() == ERROR_FILE_NOT_FOUND) {
            return 1;
        }
        Sleep(50);
    }
    return 0;
}

static int add_everyone_deny_ace(const char *path) {
    wchar_t wide_path[CUP_TEST_TEMP_PATH_SIZE];
    BYTE everyone_buffer[SECURITY_MAX_SID_SIZE];
    DWORD everyone_size = sizeof(everyone_buffer);
    PSECURITY_DESCRIPTOR descriptor = NULL;
    PACL current_dacl = NULL;
    PACL updated_dacl = NULL;
    EXPLICIT_ACCESSW access;
    DWORD status;

    if (MultiByteToWideChar(CP_UTF8,
                            MB_ERR_INVALID_CHARS,
                            path,
                            -1,
                            wide_path,
                            CUP_TEST_TEMP_PATH_SIZE) <= 0 ||
        !CreateWellKnownSid(WinWorldSid, NULL, everyone_buffer, &everyone_size)) {
        return 0;
    }
    status = GetNamedSecurityInfoW(wide_path,
                                   SE_FILE_OBJECT,
                                   DACL_SECURITY_INFORMATION,
                                   NULL,
                                   NULL,
                                   &current_dacl,
                                   NULL,
                                   &descriptor);
    if (status != ERROR_SUCCESS || current_dacl == NULL) {
        if (descriptor != NULL) {
            LocalFree(descriptor);
        }
        return 0;
    }

    memset(&access, 0, sizeof(access));
    /* Preserve the read rights used by the privacy probe while adding a
     * deliberately non-canonical deny ACE for the repair test. */
    access.grfAccessPermissions = FILE_WRITE_DATA;
    access.grfAccessMode = DENY_ACCESS;
    access.grfInheritance = NO_INHERITANCE;
    access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    access.Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
    access.Trustee.ptstrName = (LPWSTR)everyone_buffer;
    status = SetEntriesInAclW(1, &access, current_dacl, &updated_dacl);
    if (status == ERROR_SUCCESS) {
        status = SetNamedSecurityInfoW(wide_path,
                                       SE_FILE_OBJECT,
                                       DACL_SECURITY_INFORMATION |
                                           PROTECTED_DACL_SECURITY_INFORMATION,
                                       NULL,
                                       NULL,
                                       updated_dacl,
                                       NULL);
    }

    if (updated_dacl != NULL) {
        LocalFree(updated_dacl);
    }
    LocalFree(descriptor);
    return status == ERROR_SUCCESS;
}

static void test_home_and_process_identity(void) {
    char buffer[CUP_TEST_TEMP_PATH_SIZE];
    char expected[CUP_TEST_TEMP_PATH_SIZE];
    char volume[CUP_TEST_TEMP_PATH_SIZE];
    DWORD length;

    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, system_get_home_dir(NULL, 1));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, system_get_home_dir(buffer, 0));

    TEST_ASSERT_EQUAL_INT(0, _putenv_s("USERPROFILE", temp_dir));
    length = GetFullPathNameA(temp_dir, (DWORD)sizeof(expected), expected, NULL);
    TEST_ASSERT_TRUE(length > 0 && length < sizeof(expected));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_BUFFER_TOO_SMALL, system_get_home_dir(buffer, 2));
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_get_home_dir(buffer, sizeof(buffer)));
    TEST_ASSERT_TRUE(path_equal(expected, buffer));
    TEST_ASSERT_NULL(strchr(buffer, '\\'));
    TEST_ASSERT_TRUE(system_get_process_id() > 0);

    TEST_ASSERT_EQUAL_INT(0, _putenv_s("USERPROFILE", ""));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM, system_get_home_dir(buffer, sizeof(buffer)));
    TEST_ASSERT_EQUAL_INT(0, _putenv_s("USERPROFILE", "relative-profile"));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM, system_get_home_dir(buffer, sizeof(buffer)));
    {
        static const wchar_t malformed_profile[] = {(wchar_t)0xd800, L'\0'};

        TEST_ASSERT_TRUE(SetEnvironmentVariableW(L"USERPROFILE", malformed_profile));
        TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM, system_get_home_dir(buffer, sizeof(buffer)));
    }

    TEST_ASSERT_TRUE(GetVolumePathNameA(expected, volume, (DWORD)sizeof(volume)));
    TEST_ASSERT_EQUAL_INT(0, _putenv_s("USERPROFILE", volume));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM, system_get_home_dir(buffer, sizeof(buffer)));

    TEST_ASSERT_EQUAL_INT(0,
                          _putenv_s("USERPROFILE", had_profile ? original_profile : ""));
}

static void test_utf_and_identity_contract(void) {
    static const char malformed_utf8[] = {(char)0xc3, '(', '\0'};
    wchar_t wide[8];
    wchar_t protocol_wide[64];
    SystemPathKind kind;
    SystemPathIdentity first = {1u, 2u, 3u, SYSTEM_PATH_REGULAR_FILE, 1};
    SystemPathIdentity second = first;

    TEST_ASSERT_EQUAL_INT(CUP_ERR_BUFFER_TOO_SMALL,
                          windows_utf8_to_wide("x", wide, 1));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          windows_utf8_to_wide(malformed_utf8, wide, 8));
    TEST_ASSERT_EQUAL_INT(
        CUP_OK,
        windows_utf8_to_wide("C:/Users/Test/.cup",
                             protocol_wide,
                             sizeof(protocol_wide) / sizeof(protocol_wide[0])));
    TEST_ASSERT_EQUAL_INT(0, wcscmp(L"C:/Users/Test/.cup", protocol_wide));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          system_get_path_kind(malformed_utf8, &kind));

    TEST_ASSERT_TRUE(system_path_identity_equal(&first, &second));
    second.object_high++;
    TEST_ASSERT_FALSE(system_path_identity_equal(&first, &second));
}

static void test_paths_permissions_and_traversal(void) {
    char directory[CUP_TEST_TEMP_PATH_SIZE];
    char nested[CUP_TEST_TEMP_PATH_SIZE];
    char executable[CUP_TEST_TEMP_PATH_SIZE];
    char script[CUP_TEST_TEMP_PATH_SIZE];
    char batch[CUP_TEST_TEMP_PATH_SIZE];
    SystemPathKind kind;
    long long size;
    size_t count = 0;
    int value;

    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_INVALID_INPUT,
        system_get_path_kind("\\\\?\\C:relative", &kind));
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_INVALID_INPUT,
        system_get_path_kind("\\\\?\\GLOBALROOT\\Device\\HarddiskVolume1", &kind));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          system_get_path_kind("C:/cup/file:stream", &kind));

    build_path(directory, sizeof(directory), "paths");
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_make_directory(directory));
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_make_directory(directory));
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_get_path_kind(directory, &kind));
    TEST_ASSERT_EQUAL_INT(SYSTEM_PATH_DIRECTORY, kind);

    TEST_ASSERT_TRUE(snprintf(executable, sizeof(executable), "%s/tool.EXE", directory) > 0);
    TEST_ASSERT_TRUE(snprintf(script, sizeof(script), "%s/tool.ps1", directory) > 0);
    TEST_ASSERT_TRUE(snprintf(batch, sizeof(batch), "%s/wrapper.cmd", directory) > 0);
    write_text(executable, "binary");
    write_text(script, "script");
    write_text(batch, "@echo off\r\n");

    TEST_ASSERT_EQUAL_INT(CUP_OK, system_get_path_kind(executable, &kind));
    TEST_ASSERT_EQUAL_INT(SYSTEM_PATH_REGULAR_FILE, kind);
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_file_size(executable, &size));
    TEST_ASSERT_EQUAL_INT(6, size);

    TEST_ASSERT_EQUAL_INT(CUP_OK, system_set_read_only(script, 1));
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_is_read_only(script, &value));
    TEST_ASSERT_TRUE(value);
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_set_read_only(script, 0));
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_is_read_only(script, &value));
    TEST_ASSERT_FALSE(value);

    TEST_ASSERT_EQUAL_INT(CUP_OK, system_set_executable(executable, 1));
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_is_executable(executable, &value));
    TEST_ASSERT_TRUE(value);
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_set_executable(batch, 1));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM, system_set_executable(script, 1));
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_set_executable(script, 0));
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_is_executable(script, &value));
    TEST_ASSERT_FALSE(value);

    TEST_ASSERT_TRUE(snprintf(nested, sizeof(nested), "%s/nested", directory) > 0);
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_make_directory(nested));
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_list_directory(directory, count_entry, &count));
    TEST_ASSERT_EQUAL_size_t(4, count);
    count = 0;
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_walk_directory(directory, count_entry, &count));
    TEST_ASSERT_EQUAL_size_t(4, count);
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INTERRUPT,
                          system_list_directory(directory, reject_entry, NULL));

    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, system_get_path_kind(NULL, &kind));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, system_get_path_kind(executable, NULL));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          system_list_directory(directory, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM,
                          system_list_directory(executable, count_entry, &count));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM, system_is_read_only("missing", &value));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM, system_set_read_only("missing", 1));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, system_set_read_only(script, 2));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, system_set_executable(executable, -1));
}

static void test_regular_file_missing_contract(void) {
    char existing_parent[CUP_TEST_TEMP_PATH_SIZE];
    char missing_file[CUP_TEST_TEMP_PATH_SIZE];
    char missing_parent_file[CUP_TEST_TEMP_PATH_SIZE];
    FILE *file = NULL;
    SystemPathIdentity identity;
    uint64_t size = 99;
    int missing = 0;

    build_path(existing_parent, sizeof(existing_parent), "open-existing-parent");
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_make_directory(existing_parent));
    TEST_ASSERT_TRUE(snprintf(missing_file,
                              sizeof(missing_file),
                              "%s/missing.txt",
                              existing_parent) > 0);
    build_path(missing_parent_file,
               sizeof(missing_parent_file),
               "open-missing-parent/missing.txt");

    memset(&identity, 0xff, sizeof(identity));
    TEST_ASSERT_EQUAL_INT(
        CUP_OK,
        system_open_regular_file(missing_file, &file, &identity, &size, &missing));
    TEST_ASSERT_TRUE(missing);
    TEST_ASSERT_NULL(file);
    TEST_ASSERT_FALSE(identity.valid);
    TEST_ASSERT_EQUAL_UINT64(0, size);

    memset(&identity, 0xff, sizeof(identity));
    size = 99;
    missing = 0;
    TEST_ASSERT_EQUAL_INT(
        CUP_OK,
        system_open_regular_file(
            missing_parent_file, &file, &identity, &size, &missing));
    TEST_ASSERT_TRUE(missing);
    TEST_ASSERT_NULL(file);
    TEST_ASSERT_FALSE(identity.valid);
    TEST_ASSERT_EQUAL_UINT64(0, size);

    TEST_ASSERT_EQUAL_INT(CUP_OK, system_remove_directory(existing_parent));
}

static void test_reparse_points_are_not_followed(void) {
    char root[CUP_TEST_TEMP_PATH_SIZE];
    char external[CUP_TEST_TEMP_PATH_SIZE];
    char sentinel[CUP_TEST_TEMP_PATH_SIZE];
    char junction[CUP_TEST_TEMP_PATH_SIZE];
    SystemPathKind kind;
    int exists;

    build_path(root, sizeof(root), "junction-tree");
    build_path(external, sizeof(external), "junction-target");
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_make_directory(root));
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_make_directory(external));
    TEST_ASSERT_TRUE(snprintf(sentinel, sizeof(sentinel), "%s/sentinel.txt", external) > 0);
    write_text(sentinel, "preserve");
    TEST_ASSERT_TRUE(snprintf(junction, sizeof(junction), "%s/external", root) > 0);
    TEST_ASSERT_TRUE(test_create_directory_junction(junction, external));

    TEST_ASSERT_EQUAL_INT(CUP_OK, system_get_path_kind(junction, &kind));
    TEST_ASSERT_EQUAL_INT(SYSTEM_PATH_LINK, kind);
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_remove_tree(root, NULL));
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_path_exists(root, &exists));
    TEST_ASSERT_FALSE(exists);
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_path_exists(sentinel, &exists));
    TEST_ASSERT_TRUE(exists);
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_remove_tree(external, NULL));
}

static void test_parent_reparse_components_are_rejected(void) {
    char root[CUP_TEST_TEMP_PATH_SIZE];
    char external[CUP_TEST_TEMP_PATH_SIZE];
    char external_child[CUP_TEST_TEMP_PATH_SIZE];
    char linked_parent[CUP_TEST_TEMP_PATH_SIZE];
    char linked_child[CUP_TEST_TEMP_PATH_SIZE];
    char linked_new_file[CUP_TEST_TEMP_PATH_SIZE];
    char temporary[CUP_TEST_TEMP_PATH_SIZE];
    char temporary_directory[CUP_TEST_TEMP_PATH_SIZE];
    char safe_source[CUP_TEST_TEMP_PATH_SIZE];
    char safe_destination[CUP_TEST_TEMP_PATH_SIZE];
    FILE *file = NULL;
    SystemCommitState state;
    SystemLock lock = {0};
    SystemPathKind kind;
    int exists;

    build_path(root, sizeof(root), "parent-junction-root");
    build_path(external, sizeof(external), "parent-junction-target");
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_make_directory(root));
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_make_directory(external));
    TEST_ASSERT_TRUE(snprintf(external_child,
                              sizeof(external_child),
                              "%s/sentinel.txt",
                              external) > 0);
    write_text(external_child, "preserve");
    TEST_ASSERT_TRUE(snprintf(linked_parent, sizeof(linked_parent), "%s/link", root) > 0);
    TEST_ASSERT_TRUE(test_create_directory_junction(linked_parent, external));
    TEST_ASSERT_TRUE(snprintf(linked_child,
                              sizeof(linked_child),
                              "%s/sentinel.txt",
                              linked_parent) > 0);
    TEST_ASSERT_TRUE(snprintf(linked_new_file,
                              sizeof(linked_new_file),
                              "%s/new.txt",
                              linked_parent) > 0);

    /* Inspection may observe through a parent junction; trusted traversal may not mutate it. */
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_get_path_kind(linked_child, &kind));
    TEST_ASSERT_EQUAL_INT(SYSTEM_PATH_REGULAR_FILE, kind);
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM,
                          system_open_regular_file(linked_child,
                                                   &file,
                                                   &(SystemPathIdentity){0},
                                                   &(uint64_t){0},
                                                   &(int){0}));
    TEST_ASSERT_NULL(file);
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM, system_set_read_only(linked_child, 1));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM,
                          system_create_file_exclusive(linked_new_file, &file));
    TEST_ASSERT_NULL(file);
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_FILESYSTEM,
        system_create_temp_file(
            linked_parent, "file", temporary, sizeof(temporary), &file));
    TEST_ASSERT_NULL(file);
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_FILESYSTEM,
        system_create_temp_directory(linked_parent,
                                     "directory",
                                     temporary_directory,
                                     sizeof(temporary_directory)));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM, system_remove_file(linked_child));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM, system_sync_parent_directory(linked_child));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM,
                          system_lock_acquire(&lock,
                                              linked_new_file,
                                              SYSTEM_LOCK_EXCLUSIVE));
    TEST_ASSERT_FALSE(lock.active);

    build_path(safe_source, sizeof(safe_source), "parent-junction-source.txt");
    build_path(safe_destination, sizeof(safe_destination), "parent-junction-destination.txt");
    write_text(safe_source, "source");
    state = SYSTEM_COMMIT_DURABLE;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM,
                          system_move_path(safe_source, linked_new_file, &state));
    TEST_ASSERT_EQUAL_INT(SYSTEM_COMMIT_NOT_APPLIED, state);
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_path_exists(safe_source, &exists));
    TEST_ASSERT_TRUE(exists);

    state = SYSTEM_COMMIT_DURABLE;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM,
                          system_move_path(linked_child, safe_destination, &state));
    TEST_ASSERT_EQUAL_INT(SYSTEM_COMMIT_NOT_APPLIED, state);
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_path_exists(safe_destination, &exists));
    TEST_ASSERT_FALSE(exists);
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_path_exists(external_child, &exists));
    TEST_ASSERT_TRUE(exists);

    TEST_ASSERT_EQUAL_INT(CUP_OK, system_remove_file(safe_source));
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_remove_tree(root, NULL));
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_path_exists(external_child, &exists));
    TEST_ASSERT_TRUE(exists);
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_remove_tree(external, NULL));
}

static void test_identity_bound_path_removal(void) {
    char target[CUP_TEST_TEMP_PATH_SIZE];
    char original[CUP_TEST_TEMP_PATH_SIZE];
    char child[CUP_TEST_TEMP_PATH_SIZE];
    SystemPathIdentity identity;
    SystemPathIdentity invalid = {0};
    int exists;

    build_path(target, sizeof(target), "identity-tree");
    build_path(original, sizeof(original), "identity-tree-original");
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_make_directory(target));
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_get_path_identity(target, &identity));
    TEST_ASSERT_EQUAL_INT(0, rename(target, original));
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_make_directory(target));
    TEST_ASSERT_TRUE(snprintf(child, sizeof(child), "%s/child.txt", target) > 0);
    write_text(child, "foreign");

    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_TRANSACTION, system_remove_path_if_identity(target, &identity, NULL));
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_path_exists(child, &exists));
    TEST_ASSERT_TRUE(exists);
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_INVALID_INPUT, system_remove_path_if_identity(target, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_INVALID_INPUT, system_remove_path_if_identity(target, &invalid, NULL));

    TEST_ASSERT_EQUAL_INT(CUP_OK, system_get_path_identity(target, &identity));
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_INTERRUPT, system_remove_path_if_identity(target, &identity, always_cancel));
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_path_exists(target, &exists));
    TEST_ASSERT_TRUE(exists);
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_remove_path_if_identity(target, &identity, NULL));
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_path_exists(target, &exists));
    TEST_ASSERT_FALSE(exists);

    TEST_ASSERT_EQUAL_INT(CUP_OK, system_remove_tree(original, NULL));
}

static void test_handoff_primitives(void) {
    char lock_path[CUP_TEST_TEMP_PATH_SIZE];
    char parent_signal_value[32];
    char authority_value[32];
    SystemHandoff handoff = {0};
    SystemLock lock = {0};
    SECURITY_ATTRIBUTES security;
    HANDLE read_handle = NULL;
    HANDLE write_handle = NULL;
    HANDLE authority = NULL;
    int active = 1;

    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, system_handoff_active(NULL));
    TEST_ASSERT_EQUAL_INT(0, _putenv_s("USERPROFILE", temp_dir));
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_handoff_active(&active));
    TEST_ASSERT_FALSE(active);

    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          system_handoff_accept(NULL, "1", "2"));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          system_handoff_accept(&handoff, "invalid", "2"));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          system_handoff_accept(&handoff, "1", "invalid"));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          system_handoff_accept(&handoff, "1", "1"));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          system_validate_uninstall_helper_cleanup("invalid"));

    ZeroMemory(&security, sizeof(security));
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;
    TEST_ASSERT_TRUE(CreatePipe(&read_handle, &write_handle, &security, 0));
    authority = CreateEventW(NULL, TRUE, FALSE, NULL);
    TEST_ASSERT_NOT_NULL(authority);
    TEST_ASSERT_TRUE(snprintf(parent_signal_value,
                              sizeof(parent_signal_value),
                              "%llu",
                              (unsigned long long)(uintptr_t)read_handle) > 0);
    TEST_ASSERT_TRUE(snprintf(authority_value,
                              sizeof(authority_value),
                              "%llu",
                              (unsigned long long)(uintptr_t)authority) > 0);
    TEST_ASSERT_TRUE(CloseHandle(write_handle));
    write_handle = NULL;

    TEST_ASSERT_EQUAL_INT(CUP_OK,
                          system_handoff_accept(&handoff, parent_signal_value, authority_value));
    read_handle = NULL; /* consumed by system_handoff_accept */
    authority = NULL;   /* now owned by handoff */
    TEST_ASSERT_TRUE(handoff.active);

    build_path(lock_path, sizeof(lock_path), "handoff.lock");
    write_text(lock_path, "");
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          system_handoff_acquire_lock(NULL, &lock, lock_path));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          system_handoff_acquire_lock(&handoff, NULL, lock_path));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          system_handoff_acquire_lock(&handoff, &lock, NULL));
    TEST_ASSERT_EQUAL_INT(CUP_OK,
                          system_handoff_acquire_lock(&handoff, &lock, lock_path));
    TEST_ASSERT_FALSE(handoff.active);
    TEST_ASSERT_TRUE(lock.active);
    TEST_ASSERT_EQUAL_INT(SYSTEM_LOCK_EXCLUSIVE, lock.mode);
    system_handoff_release(&handoff);
    system_lock_release(&lock);
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_remove_file(lock_path));
}

static void run_uninstall_helper_cleanup_lifecycle_case(const char *copy_name,
                                                        const wchar_t *probe_mode,
                                                        DWORD expected_exit_code) {
    char executable[CUP_TEST_TEMP_PATH_SIZE];
    char copy[CUP_TEST_TEMP_PATH_SIZE];
    wchar_t wide_copy[CUP_TEST_TEMP_PATH_SIZE];
    wchar_t command[CUP_TEST_TEMP_PATH_SIZE * 2];
    SECURITY_ATTRIBUTES security;
    STARTUPINFOEXW startup;
    PROCESS_INFORMATION process;
    HANDLE cleanup_handle = INVALID_HANDLE_VALUE;
    HANDLE inherited_handles[1];
    SIZE_T attribute_size = 0;
    DWORD exit_code = 1;

    TEST_ASSERT_NOT_NULL(copy_name);
    TEST_ASSERT_NOT_NULL(probe_mode);
    TEST_ASSERT_EQUAL_INT(CUP_OK,
                          system_get_executable_path(executable, sizeof(executable)));
    build_path(copy, sizeof(copy), copy_name);
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_copy_file(executable, copy));
    TEST_ASSERT_EQUAL_INT(CUP_OK,
                          windows_utf8_to_wide(copy,
                                               wide_copy,
                                               sizeof(wide_copy) / sizeof(wide_copy[0])));

    ZeroMemory(&security, sizeof(security));
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;
    cleanup_handle = CreateFileW(wide_copy,
                                 DELETE | FILE_READ_ATTRIBUTES,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                 &security,
                                 OPEN_EXISTING,
                                 FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT |
                                     FILE_FLAG_DELETE_ON_CLOSE,
                                 NULL);
    TEST_ASSERT_NOT_EQUAL(INVALID_HANDLE_VALUE, cleanup_handle);
    TEST_ASSERT_TRUE(_snwprintf(command,
                                sizeof(command) / sizeof(command[0]),
                                L"\"%ls\" --internal-cleanup-lifecycle-probe %llu %ls",
                                wide_copy,
                                (unsigned long long)(uintptr_t)cleanup_handle,
                                probe_mode) > 0);
    command[(sizeof(command) / sizeof(command[0])) - 1] = L'\0';

    inherited_handles[0] = cleanup_handle;
    ZeroMemory(&startup, sizeof(startup));
    ZeroMemory(&process, sizeof(process));
    startup.StartupInfo.cb = sizeof(startup);
    InitializeProcThreadAttributeList(NULL, 1, 0, &attribute_size);
    TEST_ASSERT_TRUE(attribute_size > 0);
    startup.lpAttributeList = HeapAlloc(GetProcessHeap(), 0, attribute_size);
    TEST_ASSERT_NOT_NULL(startup.lpAttributeList);
    TEST_ASSERT_TRUE(InitializeProcThreadAttributeList(
        startup.lpAttributeList, 1, 0, &attribute_size));
    TEST_ASSERT_TRUE(UpdateProcThreadAttribute(startup.lpAttributeList,
                                               0,
                                               PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                               inherited_handles,
                                               sizeof(inherited_handles),
                                               NULL,
                                               NULL));
    TEST_ASSERT_TRUE(CreateProcessW(wide_copy,
                                    command,
                                    NULL,
                                    NULL,
                                    TRUE,
                                    CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT,
                                    NULL,
                                    NULL,
                                    &startup.StartupInfo,
                                    &process));
    /*
     * Keep the parent cleanup handle until the process object is signaled. This is the ordering the
     * production cleanup carrier must reproduce; closing it during child teardown is too early.
     */
    TEST_ASSERT_EQUAL_UINT32(WAIT_OBJECT_0, WaitForSingleObject(process.hProcess, 10000));
    TEST_ASSERT_TRUE(GetExitCodeProcess(process.hProcess, &exit_code));
    TEST_ASSERT_TRUE(CloseHandle(cleanup_handle));
    cleanup_handle = INVALID_HANDLE_VALUE;
    TEST_ASSERT_TRUE(CloseHandle(process.hThread));
    TEST_ASSERT_TRUE(CloseHandle(process.hProcess));
    DeleteProcThreadAttributeList(startup.lpAttributeList);
    HeapFree(GetProcessHeap(), 0, startup.lpAttributeList);
    TEST_ASSERT_EQUAL_UINT32(expected_exit_code, exit_code);
    TEST_ASSERT_TRUE(wait_for_path_missing(copy));
}

static void test_uninstall_helper_cleanup_lifecycle(void) {
    run_uninstall_helper_cleanup_lifecycle_case(
        "cleanup-lifecycle-success-probe.exe", L"success", 0);
    run_uninstall_helper_cleanup_lifecycle_case(
        "cleanup-lifecycle-failure-probe.exe", L"failure", 7);
}

static void test_handoff_helper_start(void) {
    char executable[CUP_TEST_TEMP_PATH_SIZE];
    char helper[CUP_TEST_TEMP_PATH_SIZE];
    char marker[CUP_TEST_TEMP_PATH_SIZE];
    char lock_path[CUP_TEST_TEMP_PATH_SIZE];
    char protocol_root[CUP_TEST_TEMP_PATH_SIZE];
    char protocol_detached[CUP_TEST_TEMP_PATH_SIZE];
    char contents[CUP_TEST_TEMP_PATH_SIZE * 3];
    wchar_t wide_executable[CUP_TEST_TEMP_PATH_SIZE];
    wchar_t wide_helper[CUP_TEST_TEMP_PATH_SIZE];
    wchar_t wide_marker[CUP_TEST_TEMP_PATH_SIZE];
    wchar_t wide_lock[CUP_TEST_TEMP_PATH_SIZE];
    wchar_t wide_root[CUP_TEST_TEMP_PATH_SIZE];
    wchar_t wide_detached[CUP_TEST_TEMP_PATH_SIZE];
    wchar_t command[CUP_TEST_TEMP_PATH_SIZE * 6];
    STARTUPINFOW startup;
    PROCESS_INFORMATION parent;
    DWORD exit_code = 1;
    char *line;
    SystemLock lock = {0};
    int active = 0;
    int written;

    TEST_ASSERT_EQUAL_INT(0, _putenv_s("USERPROFILE", temp_dir));
    TEST_ASSERT_EQUAL_INT(CUP_OK,
                          system_get_executable_path(executable, sizeof(executable)));
    build_path(helper, sizeof(helper), "handoff-helper-probe.exe");
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_copy_file(executable, helper));
    build_path(marker, sizeof(marker), "handoff-started.txt");
    build_path(lock_path, sizeof(lock_path), "handoff-start.lock");
    build_path(protocol_root, sizeof(protocol_root), "protocol root");
    build_path(protocol_detached, sizeof(protocol_detached), "protocol detached");
    TEST_ASSERT_EQUAL_INT(CUP_OK, path_normalize(protocol_root));
    TEST_ASSERT_EQUAL_INT(CUP_OK, path_normalize(protocol_detached));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          system_start_update_helper(NULL, temp_dir, "token", &lock));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          system_start_update_helper(executable, NULL, "token", &lock));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          system_start_update_helper(executable, temp_dir, NULL, &lock));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          system_start_uninstall_helper(
                              executable, temp_dir, NULL, "token", &lock));

    TEST_ASSERT_EQUAL_INT(CUP_OK,
                          system_lock_acquire(&lock, lock_path, SYSTEM_LOCK_EXCLUSIVE));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM,
                          system_start_update_helper(
                              "C:/cup-missing-handoff-helper.exe", temp_dir, "token", &lock));
    TEST_ASSERT_TRUE(lock.active);
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_handoff_active(&active));
    TEST_ASSERT_FALSE(active);
    system_lock_release(&lock);

    /*
     * A successful handoff cannot be tested with this Unity process as the
     * parent: production keeps the parent-lifetime pipe open until that process
     * exits. Run the real start in a short-lived parent subprocess so the helper
     * executes the same handoff wait as production.
     */
    TEST_ASSERT_EQUAL_INT(CUP_OK,
                          windows_utf8_to_wide_process_path(
                              executable, wide_executable, CUP_TEST_TEMP_PATH_SIZE));
    TEST_ASSERT_EQUAL_INT(CUP_OK,
                          windows_utf8_to_wide(helper, wide_helper, CUP_TEST_TEMP_PATH_SIZE));
    TEST_ASSERT_EQUAL_INT(CUP_OK,
                          windows_utf8_to_wide(marker, wide_marker, CUP_TEST_TEMP_PATH_SIZE));
    TEST_ASSERT_EQUAL_INT(CUP_OK,
                          windows_utf8_to_wide(lock_path, wide_lock, CUP_TEST_TEMP_PATH_SIZE));
    TEST_ASSERT_EQUAL_INT(CUP_OK,
                          windows_utf8_to_wide(protocol_root, wide_root, CUP_TEST_TEMP_PATH_SIZE));
    TEST_ASSERT_EQUAL_INT(CUP_OK,
                          windows_utf8_to_wide(
                              protocol_detached, wide_detached, CUP_TEST_TEMP_PATH_SIZE));
    written = _snwprintf(command,
                         sizeof(command) / sizeof(command[0]),
                         L"\"%ls\" --internal-handoff-parent-probe \"%ls\" \"%ls\" "
                         L"\"%ls\" \"%ls\" handoff-token \"%ls\"",
                         wide_executable,
                         wide_helper,
                         wide_lock,
                         wide_root,
                         wide_detached,
                         wide_marker);
    TEST_ASSERT_TRUE(written > 0 &&
                     (size_t)written < sizeof(command) / sizeof(command[0]));

    ZeroMemory(&startup, sizeof(startup));
    ZeroMemory(&parent, sizeof(parent));
    startup.cb = sizeof(startup);
    TEST_ASSERT_TRUE(CreateProcessW(wide_executable,
                                    command,
                                    NULL,
                                    NULL,
                                    FALSE,
                                    CREATE_NO_WINDOW,
                                    NULL,
                                    NULL,
                                    &startup,
                                    &parent));
    /* The outer probe timeout must exceed the production carrier readiness bound and its bounded
     * failure cleanup so the test observes the parent's result instead of racing it. */
    TEST_ASSERT_EQUAL_UINT32(WAIT_OBJECT_0, WaitForSingleObject(parent.hProcess, 60000));
    TEST_ASSERT_TRUE(GetExitCodeProcess(parent.hProcess, &exit_code));
    TEST_ASSERT_TRUE(CloseHandle(parent.hThread));
    TEST_ASSERT_TRUE(CloseHandle(parent.hProcess));
    if (exit_code != 0) {
        int diagnostic_exists = 0;

        if (system_path_exists(marker, &diagnostic_exists) == CUP_OK && diagnostic_exists) {
            read_text(marker, contents, sizeof(contents));
            fprintf(stderr, "Handoff parent probe failure:\n%s", contents);
        }
    }
    TEST_ASSERT_EQUAL_UINT32(0, exit_code);

    TEST_ASSERT_TRUE(wait_for_path(marker));
    /* The marker pathname can appear before the helper finishes writing it.
     * Cleanup of the temporary executable proves that the helper has exited. */
    TEST_ASSERT_TRUE(wait_for_path_missing(helper));
    read_text(marker, contents, sizeof(contents));
    line = strtok(contents, "\n");
    TEST_ASSERT_EQUAL_STRING("--internal-uninstall-helper", line);
    line = strtok(NULL, "\n");
    TEST_ASSERT_EQUAL_STRING(protocol_root, line);
    line = strtok(NULL, "\n");
    TEST_ASSERT_EQUAL_STRING(protocol_detached, line);
    line = strtok(NULL, "\n");
    TEST_ASSERT_EQUAL_STRING("handoff-token", line);
    TEST_ASSERT_NOT_NULL(strtok(NULL, "\n"));
    TEST_ASSERT_NOT_NULL(strtok(NULL, "\n"));
    TEST_ASSERT_NOT_NULL(strtok(NULL, "\n"));
    line = strtok(NULL, "\n");
    TEST_ASSERT_EQUAL_STRING("handles=valid", line);
    line = strtok(NULL, "\n");
    TEST_ASSERT_EQUAL_STRING("handoff=accepted", line);
    TEST_ASSERT_NULL(strtok(NULL, "\n"));
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_handoff_active(&active));
    TEST_ASSERT_FALSE(active);

    TEST_ASSERT_EQUAL_INT(CUP_OK, system_remove_file(marker));
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_remove_file(lock_path));
}

static void test_copy_replace_and_temporary_objects(void) {
    char source[CUP_TEST_TEMP_PATH_SIZE];
    char copy[CUP_TEST_TEMP_PATH_SIZE];
    char moved[CUP_TEST_TEMP_PATH_SIZE];
    char replacement[CUP_TEST_TEMP_PATH_SIZE];
    char identity_target[CUP_TEST_TEMP_PATH_SIZE];
    char identity_original[CUP_TEST_TEMP_PATH_SIZE];
    char identity_source[CUP_TEST_TEMP_PATH_SIZE];
    char exclusive[CUP_TEST_TEMP_PATH_SIZE];
    char missing_owner[CUP_TEST_TEMP_PATH_SIZE];
    char temporary[CUP_TEST_TEMP_PATH_SIZE];
    char temporary_directory[CUP_TEST_TEMP_PATH_SIZE];
    char unique[CUP_TEST_TEMP_PATH_SIZE];
    char buffer[64];
    FILE *file = NULL;
    SystemCommitState state;
    SystemPathIdentity expected_identity;
    int exists;

    build_path(source, sizeof(source), "source.exe");
    build_path(copy, sizeof(copy), "copy.exe");
    build_path(moved, sizeof(moved), "moved.exe");
    build_path(replacement, sizeof(replacement), "replacement.exe");
    build_path(identity_target, sizeof(identity_target), "identity-target.exe");
    build_path(identity_original, sizeof(identity_original), "identity-original.exe");
    build_path(identity_source, sizeof(identity_source), "identity-source.exe");
    build_path(exclusive, sizeof(exclusive), "exclusive.tmp");
    build_path(missing_owner, sizeof(missing_owner), "missing-temp-owner");
    write_text(source, "source-data");

    TEST_ASSERT_EQUAL_INT(CUP_OK, system_copy_file(source, copy));
    read_text(copy, buffer, sizeof(buffer));
    TEST_ASSERT_EQUAL_STRING("source-data", buffer);
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_copy_file(source, source));
    read_text(source, buffer, sizeof(buffer));
    TEST_ASSERT_EQUAL_STRING("source-data", buffer);

    state = SYSTEM_COMMIT_NOT_APPLIED;
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_move_path(copy, moved, &state));
    TEST_ASSERT_EQUAL_INT(SYSTEM_COMMIT_DURABLE, state);
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_path_exists(copy, &exists));
    TEST_ASSERT_FALSE(exists);
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_path_exists(moved, &exists));
    TEST_ASSERT_TRUE(exists);

    write_text(replacement, "new-data");
    state = SYSTEM_COMMIT_NOT_APPLIED;
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_replace_file(replacement, moved, &state));
    TEST_ASSERT_EQUAL_INT(SYSTEM_COMMIT_DURABLE, state);
    read_text(moved, buffer, sizeof(buffer));
    TEST_ASSERT_EQUAL_STRING("new-data", buffer);

    /* Identity-bound replacement requires the exact observed destination to remain present. */
    write_text(identity_target, "original");
    TEST_ASSERT_EQUAL_INT(CUP_OK,
                          system_get_path_identity(identity_target, &expected_identity));
    state = SYSTEM_COMMIT_NOT_APPLIED;
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_move_path(identity_target, identity_original, &state));
    TEST_ASSERT_EQUAL_INT(SYSTEM_COMMIT_DURABLE, state);
    write_text(identity_source, "new-value");
    state = SYSTEM_COMMIT_NOT_APPLIED;
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_TRANSACTION,
        system_replace_file_if_identity(
            identity_source, identity_target, &expected_identity, &state));
    TEST_ASSERT_EQUAL_INT(SYSTEM_COMMIT_NOT_APPLIED, state);
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_path_exists(identity_target, &exists));
    TEST_ASSERT_FALSE(exists);
    write_text(identity_target, "foreign");
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_TRANSACTION,
        system_replace_file_if_identity(
            identity_source, identity_target, &expected_identity, &state));
    read_text(identity_target, buffer, sizeof(buffer));
    TEST_ASSERT_EQUAL_STRING("foreign", buffer);
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_remove_file(identity_source));
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_remove_file(identity_target));
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_remove_file(identity_original));

    /* Source-identity-bound moves reject a pathname that no longer names the observed object. */
    write_text(identity_source, "move-old");
    TEST_ASSERT_EQUAL_INT(CUP_OK,
                          system_get_path_identity(identity_source, &expected_identity));
    state = SYSTEM_COMMIT_NOT_APPLIED;
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_move_path(identity_source, identity_original, &state));
    TEST_ASSERT_EQUAL_INT(SYSTEM_COMMIT_DURABLE, state);
    write_text(identity_source, "move-foreign");
    state = SYSTEM_COMMIT_NOT_APPLIED;
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_TRANSACTION,
        system_move_path_if_identity(
            identity_source, identity_target, &expected_identity, &state));
    TEST_ASSERT_EQUAL_INT(SYSTEM_COMMIT_NOT_APPLIED, state);
    read_text(identity_source, buffer, sizeof(buffer));
    TEST_ASSERT_EQUAL_STRING("move-foreign", buffer);
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_path_exists(identity_target, &exists));
    TEST_ASSERT_FALSE(exists);

    TEST_ASSERT_EQUAL_INT(CUP_OK,
                          system_get_path_identity(identity_source, &expected_identity));
    {
        SystemPathIdentity wrong_kind = expected_identity;

        wrong_kind.kind = SYSTEM_PATH_LINK;
        state = SYSTEM_COMMIT_NOT_APPLIED;
        TEST_ASSERT_EQUAL_INT(
            CUP_ERR_INVALID_INPUT,
            system_move_path_if_identity(
                identity_source, identity_target, &wrong_kind, &state));
        TEST_ASSERT_EQUAL_INT(SYSTEM_COMMIT_NOT_APPLIED, state);
    }
    state = SYSTEM_COMMIT_NOT_APPLIED;
    TEST_ASSERT_EQUAL_INT(
        CUP_OK,
        system_move_path_if_identity(
            identity_source, identity_target, &expected_identity, &state));
    TEST_ASSERT_EQUAL_INT(SYSTEM_COMMIT_DURABLE, state);
    read_text(identity_target, buffer, sizeof(buffer));
    TEST_ASSERT_EQUAL_STRING("move-foreign", buffer);
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_remove_file(identity_target));
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_remove_file(identity_original));

    /* Directory sources use the same source-identity contract. */
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_make_directory(identity_source));
    TEST_ASSERT_EQUAL_INT(CUP_OK,
                          system_get_path_identity(identity_source, &expected_identity));
    state = SYSTEM_COMMIT_NOT_APPLIED;
    TEST_ASSERT_EQUAL_INT(
        CUP_OK,
        system_move_path_if_identity(
            identity_source, identity_target, &expected_identity, &state));
    TEST_ASSERT_EQUAL_INT(SYSTEM_COMMIT_DURABLE, state);
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_path_exists(identity_source, &exists));
    TEST_ASSERT_FALSE(exists);
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_remove_directory(identity_target));

    TEST_ASSERT_EQUAL_INT(CUP_OK, system_create_file_exclusive(exclusive, &file));
    TEST_ASSERT_NOT_NULL(file);
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_sync_file(file));
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
    file = NULL;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_LOCK, system_create_file_exclusive(exclusive, &file));

    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_TEMPORARY,
        system_create_temp_file(
            missing_owner, "file", temporary, sizeof(temporary), &file));
    TEST_ASSERT_NULL(file);
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_TEMPORARY,
        system_create_temp_directory(missing_owner,
                                     "directory",
                                     temporary_directory,
                                     sizeof(temporary_directory)));
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_FILESYSTEM,
        system_create_temp_file(exclusive, "file", temporary, sizeof(temporary), &file));
    TEST_ASSERT_NULL(file);
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_FILESYSTEM,
        system_create_temp_directory(
            exclusive, "directory", temporary_directory, sizeof(temporary_directory)));

    TEST_ASSERT_EQUAL_INT(CUP_OK,
                          system_create_temp_file(
                              temp_dir, "file", temporary, sizeof(temporary), &file));
    TEST_ASSERT_NOT_NULL(file);
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
    file = NULL;
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_INVALID_INPUT,
        system_create_temp_file(temp_dir, "../escape", temporary, sizeof(temporary), &file));
    TEST_ASSERT_NULL(file);
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM, system_set_executable(temporary, 1));
    TEST_ASSERT_EQUAL_INT(CUP_OK,
                          system_create_temp_directory(temp_dir,
                                                       "directory",
                                                       temporary_directory,
                                                       sizeof(temporary_directory)));
    TEST_ASSERT_EQUAL_INT(CUP_OK,
                          system_directory_is_private(temporary_directory, &exists));
    TEST_ASSERT_TRUE(exists);
    assert_private_acl_is_inheritable(temporary_directory);
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_INVALID_INPUT,
        system_create_temp_directory(
            temp_dir, "../escape", temporary_directory, sizeof(temporary_directory)));
    TEST_ASSERT_EQUAL_INT(CUP_OK,
                          system_make_unique_temp_path(
                              temp_dir, "unique", unique, sizeof(unique)));
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_path_exists(unique, &exists));
    TEST_ASSERT_FALSE(exists);
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_sync_parent_directory(moved));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, system_sync_parent_directory(""));

    TEST_ASSERT_EQUAL_INT(CUP_OK, system_remove_file(exclusive));
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_remove_file(temporary));
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_remove_directory(temporary_directory));
}

static void test_shared_script_primitives(void) {
    char chain[MAX_PATH_LEN];
    char parent[MAX_PATH_LEN];
    char exclusive[MAX_PATH_LEN];
    char contents[MAX_PATH_LEN];
    char keep[MAX_PATH_LEN];
    char remove[MAX_PATH_LEN];
    char lock_path[MAX_PATH_LEN];
    char buffer[32];
    FILE *lock_reader = NULL;
    SystemCommitState state;
    SystemPathIdentity identity;
    SystemPathIdentity reader_identity;
    SystemLock lock = {0};
    SystemPathKind kind;
    uint64_t reader_size = 0;
    int missing = 0;
    size_t size;

    build_path(parent, sizeof(parent), "chain");
    TEST_ASSERT_TRUE(snprintf(chain, sizeof(chain), "%s/one/two", parent) > 0);
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_check_directory_chain(chain, 1));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM, system_check_directory_chain(chain, 0));
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_make_directory_chain(chain));
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_check_directory_chain(chain, 0));

    build_path(exclusive, sizeof(exclusive), "script-exclusive");
    state = SYSTEM_COMMIT_NOT_APPLIED;
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_create_directory_exclusive(exclusive, 0700, &state));
    TEST_ASSERT_EQUAL_INT(SYSTEM_COMMIT_DURABLE, state);
    state = SYSTEM_COMMIT_DURABLE;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_LOCK,
                          system_create_directory_exclusive(exclusive, 0700, &state));
    TEST_ASSERT_EQUAL_INT(SYSTEM_COMMIT_NOT_APPLIED, state);

    build_path(contents, sizeof(contents), "contents");
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_make_directory(contents));
    TEST_ASSERT_TRUE(snprintf(keep, sizeof(keep), "%s/keep", contents) > 0);
    TEST_ASSERT_TRUE(snprintf(remove, sizeof(remove), "%s/remove", contents) > 0);
    write_text(keep, "keep");
    write_text(remove, "remove");
    TEST_ASSERT_EQUAL_INT(CUP_OK,
                          system_remove_tree_contents(contents, "keep", NULL));
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_get_path_kind(keep, &kind));
    TEST_ASSERT_EQUAL_INT(SYSTEM_PATH_REGULAR_FILE, kind);
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_get_path_kind(remove, &kind));
    TEST_ASSERT_EQUAL_INT(SYSTEM_PATH_MISSING, kind);
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INTERRUPT,
                          system_remove_tree_contents(contents, NULL, always_cancel));

    build_path(lock_path, sizeof(lock_path), "existing.lock");
    write_text(lock_path, "format=1\n");
    TEST_ASSERT_EQUAL_INT(CUP_OK,
                          system_lock_acquire_existing(
                              &lock, lock_path, SYSTEM_LOCK_SHARED));
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_lock_get_identity(&lock, &identity));
    TEST_ASSERT_TRUE(identity.valid);
    TEST_ASSERT_EQUAL_INT(SYSTEM_PATH_REGULAR_FILE, identity.kind);
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_lock_read(&lock, buffer, sizeof(buffer), &size));
    TEST_ASSERT_EQUAL_size_t(strlen("format=1\n"), size);
    buffer[size] = '\0';
    TEST_ASSERT_EQUAL_STRING("format=1\n", buffer);

    /* An exclusive SystemLock must coordinate other locks without making the
     * file contents unreadable or preventing identity-bound unlink. */
    TEST_ASSERT_EQUAL_INT(
        CUP_OK,
        system_open_regular_file(
            lock_path, &lock_reader, &reader_identity, &reader_size, &missing));
    TEST_ASSERT_FALSE(missing);
    TEST_ASSERT_TRUE(system_path_identity_equal(&identity, &reader_identity));
    TEST_ASSERT_EQUAL_UINT64(strlen("format=1\n"), reader_size);
    TEST_ASSERT_NOT_NULL(lock_reader);
    TEST_ASSERT_EQUAL_size_t(strlen("format=1\n"),
                             fread(buffer, 1, sizeof(buffer), lock_reader));
    TEST_ASSERT_EQUAL_INT(0, fclose(lock_reader));
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_remove_file_if_identity(lock_path, &identity));
    system_lock_release(&lock);
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_get_path_kind(lock_path, &kind));
    TEST_ASSERT_EQUAL_INT(SYSTEM_PATH_MISSING, kind);

    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM,
                          system_lock_acquire_existing(
                              &lock, "C:/cup-missing-existing-lock", SYSTEM_LOCK_SHARED));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          system_remove_tree_contents(contents, "../unsafe", NULL));
}

static void test_private_directory_tree_removal_and_locks(void) {
    char private_directory[CUP_TEST_TEMP_PATH_SIZE];
    char tree[CUP_TEST_TEMP_PATH_SIZE];
    char child[CUP_TEST_TEMP_PATH_SIZE];
    char lock_path[CUP_TEST_TEMP_PATH_SIZE];
    SystemLock first = {0};
    SystemLock second = {0};
    int is_private;
    int exists;

    build_path(private_directory, sizeof(private_directory), "private");
    {
        SystemCommitState state = SYSTEM_COMMIT_NOT_APPLIED;

        TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                              system_create_private_directory(NULL, &state));
        TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                              system_create_private_directory(private_directory, NULL));
        TEST_ASSERT_EQUAL_INT(CUP_OK,
                              system_create_private_directory(private_directory, &state));
        TEST_ASSERT_EQUAL_INT(SYSTEM_COMMIT_DURABLE, state);
        state = SYSTEM_COMMIT_NOT_APPLIED;
        TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM,
                              system_create_private_directory(private_directory, &state));
        TEST_ASSERT_EQUAL_INT(SYSTEM_COMMIT_NOT_APPLIED, state);
    }
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_make_private_directory(private_directory));
    TEST_ASSERT_EQUAL_INT(CUP_OK,
                          system_directory_is_private(private_directory, &is_private));
    TEST_ASSERT_TRUE(is_private);
    assert_private_acl_is_inheritable(private_directory);
    TEST_ASSERT_TRUE(add_everyone_deny_ace(private_directory));
    TEST_ASSERT_EQUAL_INT(CUP_OK,
                          system_directory_is_private(private_directory, &is_private));
    TEST_ASSERT_FALSE(is_private);
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_make_private_directory(private_directory));
    TEST_ASSERT_EQUAL_INT(CUP_OK,
                          system_directory_is_private(private_directory, &is_private));
    TEST_ASSERT_TRUE(is_private);
    assert_private_acl_is_inheritable(private_directory);
    build_path(tree, sizeof(tree), "tree");
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_make_directory(tree));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INTERRUPT, system_remove_tree(tree, always_cancel));
    TEST_ASSERT_TRUE(snprintf(child, sizeof(child), "%s/child.txt", tree) > 0);
    write_text(child, "child");
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_set_read_only(tree, 1));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM, system_remove_directory(tree));
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_is_read_only(tree, &exists));
    TEST_ASSERT_TRUE(exists);
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_set_read_only(tree, 0));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INTERRUPT, system_remove_tree(tree, always_cancel));
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_remove_tree(tree, NULL));
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_remove_tree(tree, NULL));

    build_path(lock_path, sizeof(lock_path), "cup.lock");
    TEST_ASSERT_EQUAL_INT(CUP_OK,
                          system_lock_acquire(&first, lock_path, SYSTEM_LOCK_EXCLUSIVE));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          system_lock_acquire(&first, lock_path, SYSTEM_LOCK_EXCLUSIVE));
    TEST_ASSERT_TRUE(first.active);
    TEST_ASSERT_EQUAL_INT(CUP_ERR_LOCK,
                          system_lock_acquire(&second, lock_path, SYSTEM_LOCK_SHARED));
    system_lock_release(&second);
    system_lock_release(&first);
    TEST_ASSERT_EQUAL_INT(CUP_OK,
                          system_lock_acquire(&second, lock_path, SYSTEM_LOCK_SHARED));
    system_lock_release(&second);
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_remove_file(lock_path));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM,
                          system_lock_acquire(&second, lock_path, SYSTEM_LOCK_SHARED));
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_path_exists(lock_path, &exists));
    TEST_ASSERT_FALSE(exists);

    TEST_ASSERT_EQUAL_INT(CUP_OK, system_remove_directory(private_directory));
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_remove_directory(private_directory));
}

static void write_handoff_parent_error(const char *marker,
                                       const char *stage,
                                       CupError err,
                                       DWORD native_error) {
    FILE *file;

    if (marker == NULL || stage == NULL) {
        return;
    }
    file = fopen(marker, "wb");
    if (file == NULL) {
        return;
    }
    (void)fprintf(file,
                  "parent_stage=%s\ncup_error=%d\nnative_error=%lu\n",
                  stage,
                  (int)err,
                  (unsigned long)native_error);
    (void)fclose(file);
}

static int run_handoff_parent_probe(int argc, char **argv) {
    SystemLock lock = {0};
    CupError err;
    DWORD native_error;
    int active = 0;

    if (argc != 8 || _putenv_s("CUP_TEST_HANDOFF_MARKER", argv[7]) != 0) {
        return 2;
    }
    SetLastError(ERROR_SUCCESS);
    err = system_lock_acquire(&lock, argv[3], SYSTEM_LOCK_EXCLUSIVE);
    if (err != CUP_OK) {
        write_handoff_parent_error(argv[7], "lock", err, GetLastError());
        return 3;
    }
    SetLastError(ERROR_SUCCESS);
    err = system_start_uninstall_helper(argv[2], argv[4], argv[5], argv[6], &lock);
    if (err != CUP_OK) {
        native_error = GetLastError();
        write_handoff_parent_error(argv[7], "start", err, native_error);
        system_lock_release(&lock);
        return 10 + (int)err;
    }
    if (lock.active || system_handoff_active(&active) != CUP_OK || !active) {
        write_handoff_parent_error(argv[7], "authority", CUP_ERR_TRANSACTION, GetLastError());
        return 4;
    }
    return 0;
}

static int run_handoff_probe(int argc, char **argv) {
    const char *marker = getenv("CUP_TEST_HANDOFF_MARKER");
    FILE *file;
    HANDLE wait_handle;
    HANDLE authority_handle;
    HANDLE cleanup_handle = NULL;
    DWORD flags;
    BY_HANDLE_FILE_INFORMATION information;
    SystemHandoff handoff = {0};
    CupError err;
    int uninstall;
    int i;

    if (marker == NULL || marker[0] == '\0' || argc < 6) {
        return 2;
    }
    uninstall = strcmp(argv[1], "--internal-uninstall-helper") == 0;
    if (uninstall && argc != 8) {
        return 2;
    }
    wait_handle = (HANDLE)(uintptr_t)_strtoui64(argv[argc - (uninstall ? 3 : 2)], NULL, 10);
    authority_handle =
        (HANDLE)(uintptr_t)_strtoui64(argv[argc - (uninstall ? 2 : 1)], NULL, 10);
    if (uninstall) {
        cleanup_handle = (HANDLE)(uintptr_t)_strtoui64(argv[argc - 1], NULL, 10);
    }
    if (wait_handle == NULL || authority_handle == NULL ||
        !GetHandleInformation(wait_handle, &flags) ||
        !GetHandleInformation(authority_handle, &flags)) {
        return 3;
    }
    if (uninstall &&
        (cleanup_handle == NULL || !GetHandleInformation(cleanup_handle, &flags) ||
         !GetFileInformationByHandle(cleanup_handle, &information) ||
         system_validate_uninstall_helper_cleanup(argv[argc - 1]) != CUP_OK)) {
        return 3;
    }
    err = system_handoff_accept(&handoff,
                                argv[argc - (uninstall ? 3 : 2)],
                                argv[argc - (uninstall ? 2 : 1)]);
    if (err != CUP_OK) {
        return 6;
    }
    file = fopen(marker, "wb");
    if (file == NULL) {
        system_handoff_release(&handoff);
        return 4;
    }
    for (i = 1; i < argc; ++i) {
        if (fprintf(file, "%s\n", argv[i]) < 0) {
            fclose(file);
            system_handoff_release(&handoff);
            return 5;
        }
    }
    if (fputs("handles=valid\nhandoff=accepted\n", file) == EOF || fclose(file) != 0) {
        system_handoff_release(&handoff);
        return 5;
    }
    system_handoff_release(&handoff);
    /* Helper cleanup depends on process termination, not on the operation
     * result. */
    return uninstall ? 7 : 0;
}

static int run_cleanup_lifecycle_probe(int argc, char **argv) {
    if (argc != 4) {
        return 2;
    }
    if (system_validate_uninstall_helper_cleanup(argv[2]) != CUP_OK) {
        return 3;
    }
    if (strcmp(argv[3], "success") == 0) {
        return 0;
    }
    if (strcmp(argv[3], "failure") == 0) {
        return 7;
    }
    return 4;
}

int main(int argc, char **argv) {
    DWORD length;

    if (argc > 1 && strcmp(argv[1], "--internal-cleanup-lifecycle-probe") == 0) {
        return run_cleanup_lifecycle_probe(argc, argv);
    }
    if (argc > 1 && strcmp(argv[1], "--internal-handoff-parent-probe") == 0) {
        return run_handoff_parent_probe(argc, argv);
    }
    if (argc > 1 &&
        (strcmp(argv[1], "--internal-update-helper") == 0 ||
         strcmp(argv[1], "--internal-uninstall-helper") == 0)) {
        return run_handoff_probe(argc, argv);
    }
    length = GetEnvironmentVariableA(
        "USERPROFILE", original_profile, (DWORD)sizeof(original_profile));

    had_profile = length > 0 && length < sizeof(original_profile);
    if (test_make_temp_directory(
            temp_dir, sizeof(temp_dir), "cup-system-windows-test") == NULL) {
        return 1;
    }

    UNITY_BEGIN();
    RUN_TEST(test_home_and_process_identity);
    RUN_TEST(test_utf_and_identity_contract);
    RUN_TEST(test_paths_permissions_and_traversal);
    RUN_TEST(test_regular_file_missing_contract);
    RUN_TEST(test_reparse_points_are_not_followed);
    RUN_TEST(test_parent_reparse_components_are_rejected);
    RUN_TEST(test_identity_bound_path_removal);
    RUN_TEST(test_handoff_primitives);
    RUN_TEST(test_uninstall_helper_cleanup_lifecycle);
    RUN_TEST(test_copy_replace_and_temporary_objects);
    RUN_TEST(test_shared_script_primitives);
    RUN_TEST(test_private_directory_tree_removal_and_locks);
    RUN_TEST(test_handoff_helper_start);
    {
        int result = UNITY_END();

        if (had_profile) {
            (void)_putenv_s("USERPROFILE", original_profile);
        } else {
            (void)_putenv_s("USERPROFILE", "");
        }
        (void)test_remove_tree(temp_dir);
        return result;
    }
}
