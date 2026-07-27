/*
 * Implements the complete system.h contract with native wide-character Windows APIs, including
 * locking, replacement, attributes, traversal and detached PowerShell helpers.
 */

#include "system.h"

#include "constants.h"
#include "path.h"
#include "text.h"

#include "windows_utf.h"
#include <ctype.h>
#include <windows.h>
#include <sddl.h>
#include <aclapi.h>
#include <accctrl.h>
#include <fcntl.h>
#include <io.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>

/* UTF-8 boundary conversion and native error reporting. */

static CupError utf8_to_wide_path(const char *input, wchar_t *output, size_t output_count) {
    wchar_t converted[MAX_PATH_LEN];
    wchar_t absolute[MAX_PATH_LEN];
    DWORD length;
    size_t i;
    size_t required;

    if (windows_utf8_to_wide(input, converted, MAX_PATH_LEN) != CUP_OK || output == NULL ||
        output_count == 0) {
        return CUP_ERR_INVALID_INPUT;
    }
    for (i = 0; converted[i] != L'\0'; ++i) {
        if (converted[i] == L'/') {
            converted[i] = L'\\';
        }
    }

    if (wcsncmp(converted, L"\\\\?\\", 4) == 0) {
        required = wcslen(converted) + 1;
        if (required > output_count) {
            return CUP_ERR_BUFFER_TOO_SMALL;
        }
        memcpy(output, converted, required * sizeof(*output));
        return CUP_OK;
    }
    if (wcsncmp(converted, L"\\\\.\\", 4) == 0) {
        return CUP_ERR_INVALID_INPUT;
    }

    length = GetFullPathNameW(converted, MAX_PATH_LEN, absolute, NULL);
    if (length == 0 || length >= MAX_PATH_LEN) {
        return CUP_ERR_FILESYSTEM;
    }

    if (absolute[0] == L'\\' && absolute[1] == L'\\') {
        required = 8 + wcslen(absolute + 2) + 1;
        if (required > output_count) {
            return CUP_ERR_BUFFER_TOO_SMALL;
        }
        wcscpy(output, L"\\\\?\\UNC\\");
        wcscat(output, absolute + 2);
    } else {
        required = 4 + wcslen(absolute) + 1;
        if (required > output_count) {
            return CUP_ERR_BUFFER_TOO_SMALL;
        }
        wcscpy(output, L"\\\\?\\");
        wcscat(output, absolute);
    }
    return CUP_OK;
}

/* External process arguments use ordinary absolute paths, not Win32 device prefixes. */
static CupError utf8_to_wide_process_path(const char *input,
                                           wchar_t *output,
                                           size_t output_count) {
    wchar_t converted[MAX_PATH_LEN];
    wchar_t absolute[MAX_PATH_LEN];
    DWORD length;
    size_t i;
    size_t required;

    if (windows_utf8_to_wide(input, converted, MAX_PATH_LEN) != CUP_OK || output == NULL ||
        output_count == 0) {
        return CUP_ERR_INVALID_INPUT;
    }
    for (i = 0; converted[i] != L'\0'; ++i) {
        if (converted[i] == L'/') {
            converted[i] = L'\\';
        }
    }

    if (wcsncmp(converted, L"\\\\.\\", 4) == 0) {
        return CUP_ERR_INVALID_INPUT;
    }
    if (_wcsnicmp(converted, L"\\\\?\\UNC\\", 8) == 0) {
        required = 2 + wcslen(converted + 8) + 1;
        if (required > MAX_PATH_LEN) {
            return CUP_ERR_BUFFER_TOO_SMALL;
        }
        memmove(converted + 2,
                converted + 8,
                (wcslen(converted + 8) + 1) * sizeof(*converted));
        converted[0] = L'\\';
        converted[1] = L'\\';
    } else if (wcsncmp(converted, L"\\\\?\\", 4) == 0) {
        memmove(converted,
                converted + 4,
                (wcslen(converted + 4) + 1) * sizeof(*converted));
    }

    length = GetFullPathNameW(converted, MAX_PATH_LEN, absolute, NULL);
    if (length == 0 || length >= MAX_PATH_LEN) {
        return CUP_ERR_FILESYSTEM;
    }

    required = wcslen(absolute) + 1;
    if (required > output_count) {
        return CUP_ERR_BUFFER_TOO_SMALL;
    }
    memcpy(output, absolute, required * sizeof(*output));
    return CUP_OK;
}

static CupError wide_to_utf8(const wchar_t *input, char *output, size_t output_size) {
    int written;

    if (input == NULL || output == NULL || output_size == 0 || output_size > INT_MAX) {
        return CUP_ERR_INVALID_INPUT;
    }

    written = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, input, -1, output, (int)output_size, NULL, NULL);
    return written == 0 ? CUP_ERR_FILESYSTEM : CUP_OK;
}

static void print_windows_error(const char *message, const char *path) {
    DWORD error_code = GetLastError();
    wchar_t wide_message[512];
    char error_message[1024];
    DWORD length;

    if (text_is_empty(message)) {
        message = "Windows operation";
    }

    wide_message[0] = L'\0';
    length = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                            NULL,
                            error_code,
                            0,
                            wide_message,
                            (DWORD)(sizeof(wide_message) / sizeof(wide_message[0])),
                            NULL);
    while (length > 0 && (wide_message[length - 1] == L'\r' || wide_message[length - 1] == L'\n' ||
                          wide_message[length - 1] == L' ' || wide_message[length - 1] == L'\t')) {
        wide_message[--length] = L'\0';
    }

    if (length == 0 || wide_to_utf8(wide_message, error_message, sizeof(error_message)) != CUP_OK) {
        text_format(error_message,
                    sizeof(error_message),
                    "Windows error code %lu",
                    (unsigned long)error_code);
    }

    if (text_is_empty(path)) {
        fprintf(stderr, "Error: %s failed: %s.\n", message, error_message);
    } else {
        fprintf(stderr, "Error: %s '%s' failed: %s.\n", message, path, error_message);
    }
}

static SystemPathKind path_kind_from_attributes(DWORD attributes) {
    if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        return SYSTEM_PATH_LINK;
    }
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        return SYSTEM_PATH_DIRECTORY;
    }
    return SYSTEM_PATH_REGULAR_FILE;
}

static int has_command_extension(const char *path) {
    const char *extension = strrchr(path, '.');
    static const char *const extensions[] = {".exe", ".com", ".bat", ".cmd"};
    size_t i;

    if (extension == NULL) {
        return 0;
    }

    for (i = 0; i < sizeof(extensions) / sizeof(extensions[0]); ++i) {
        const unsigned char *left = (const unsigned char *)extension;
        const unsigned char *right = (const unsigned char *)extensions[i];

        while (*left != '\0' && *right != '\0' && tolower(*left) == tolower(*right)) {
            left++;
            right++;
        }
        if (*left == '\0' && *right == '\0') {
            return 1;
        }
    }

    return 0;
}

static CupError build_temp_candidate(const char *directory,
                                     const char *prefix,
                                     const char *suffix,
                                     unsigned long attempt,
                                     char *path,
                                     size_t path_size) {
    ULONGLONG tick = GetTickCount64();
    DWORD pid = GetCurrentProcessId();

    return text_format(path,
                       path_size,
                       "%s/%s-%lu-%llu-%lu%s",
                       directory,
                       prefix,
                       (unsigned long)pid,
                       (unsigned long long)tick,
                       attempt,
                       suffix);
}

static CupError open_temp_handle(const char *directory,
                                 const char *prefix,
                                 const char *suffix,
                                 char *path,
                                 size_t path_size,
                                 HANDLE *handle) {
    unsigned long attempt;

    if (text_is_empty(directory) || text_is_empty(prefix) || text_is_empty(suffix) ||
        suffix[0] != '.' || path == NULL || path_size == 0 || handle == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    for (attempt = 0; attempt < 256; ++attempt) {
        wchar_t wide_path[MAX_PATH_LEN];

        if (build_temp_candidate(directory, prefix, suffix, attempt, path, path_size) != CUP_OK ||
            utf8_to_wide_path(path, wide_path, MAX_PATH_LEN) != CUP_OK) {
            return CUP_ERR_TEMPORARY;
        }

        *handle = CreateFileW(wide_path,
                              GENERIC_READ | GENERIC_WRITE,
                              0,
                              NULL,
                              CREATE_NEW,
                              FILE_ATTRIBUTE_TEMPORARY,
                              NULL);
        if (*handle != INVALID_HANDLE_VALUE) {
            return CUP_OK;
        }
        if (GetLastError() != ERROR_FILE_EXISTS && GetLastError() != ERROR_ALREADY_EXISTS) {
            return CUP_ERR_TEMPORARY;
        }
    }

    return CUP_ERR_TEMPORARY;
}

static CupError create_temp_file_with_suffix(const char *directory,
                                             const char *prefix,
                                             const char *suffix,
                                             char *path,
                                             size_t path_size,
                                             FILE **file) {
    HANDLE handle;
    int descriptor;

    if (file == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    *file = NULL;
    if (open_temp_handle(directory, prefix, suffix, path, path_size, &handle) != CUP_OK) {
        return CUP_ERR_TEMPORARY;
    }
    descriptor = _open_osfhandle((intptr_t)handle, _O_BINARY | _O_RDWR);
    if (descriptor == -1) {
        CloseHandle(handle);
        system_remove_file(path);
        return CUP_ERR_TEMPORARY;
    }
    *file = _fdopen(descriptor, "w+b");
    if (*file == NULL) {
        _close(descriptor);
        system_remove_file(path);
        return CUP_ERR_TEMPORARY;
    }
    return CUP_OK;
}

static CupError load_current_user(TOKEN_USER **user) {
    HANDLE token = NULL;
    DWORD size = 0;
    TOKEN_USER *value;

    if (user == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    *user = NULL;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return CUP_ERR_FILESYSTEM;
    }
    GetTokenInformation(token, TokenUser, NULL, 0, &size);
    if (size == 0 || GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
        CloseHandle(token);
        return CUP_ERR_FILESYSTEM;
    }
    value = malloc(size);
    if (value == NULL) {
        CloseHandle(token);
        return CUP_ERR_FILESYSTEM;
    }
    if (!GetTokenInformation(token, TokenUser, value, size, &size)) {
        free(value);
        CloseHandle(token);
        return CUP_ERR_FILESYSTEM;
    }
    CloseHandle(token);
    *user = value;
    return CUP_OK;
}

static CupError build_private_security_descriptor(PSECURITY_DESCRIPTOR *descriptor) {
    TOKEN_USER *user = NULL;
    LPWSTR sid_text = NULL;
    wchar_t sddl[2048];
    CupError err;
    int written;

    if (descriptor == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    *descriptor = NULL;
    err = load_current_user(&user);
    if (err != CUP_OK) {
        return err;
    }
    if (!ConvertSidToStringSidW(user->User.Sid, &sid_text)) {
        free(user);
        return CUP_ERR_FILESYSTEM;
    }
    written = _snwprintf(sddl,
                         sizeof(sddl) / sizeof(sddl[0]),
                         L"O:%lsD:P(A;OICI;FA;;;%ls)(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)",
                         sid_text,
                         sid_text);
    LocalFree(sid_text);
    free(user);
    if (written < 0 || (size_t)written >= sizeof(sddl) / sizeof(sddl[0])) {
        return CUP_ERR_BUFFER_TOO_SMALL;
    }
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            sddl, SDDL_REVISION_1, descriptor, NULL)) {
        return CUP_ERR_FILESYSTEM;
    }
    return CUP_OK;
}

static int sid_is_private_principal(PSID sid, PSID user_sid) {
    BYTE system_buffer[SECURITY_MAX_SID_SIZE];
    BYTE admin_buffer[SECURITY_MAX_SID_SIZE];
    DWORD system_size = sizeof(system_buffer);
    DWORD admin_size = sizeof(admin_buffer);

    if (EqualSid(sid, user_sid)) {
        return 1;
    }
    if (CreateWellKnownSid(WinLocalSystemSid, NULL, system_buffer, &system_size) &&
        EqualSid(sid, system_buffer)) {
        return 1;
    }
    if (CreateWellKnownSid(WinBuiltinAdministratorsSid, NULL, admin_buffer, &admin_size) &&
        EqualSid(sid, admin_buffer)) {
        return 1;
    }
    return 0;
}

static int wide_path_is_absolute(const wchar_t *path) {
    if (path == NULL) {
        return 0;
    }
    if (iswalpha(path[0]) && path[1] == L':' &&
        (path[2] == L'\\' || path[2] == L'/')) {
        return 1;
    }
    return path[0] == L'\\' && path[1] == L'\\';
}

static int wide_path_is_volume_root(const wchar_t *path) {
    wchar_t volume[MAX_PATH_LEN];
    size_t path_length;
    size_t volume_length;

    if (path == NULL || !GetVolumePathNameW(path, volume, MAX_PATH_LEN)) {
        return 0;
    }
    path_length = wcslen(path);
    volume_length = wcslen(volume);
    while (path_length > 0 && (path[path_length - 1] == L'\\' || path[path_length - 1] == L'/')) {
        path_length--;
    }
    while (volume_length > 0 &&
           (volume[volume_length - 1] == L'\\' || volume[volume_length - 1] == L'/')) {
        volume_length--;
    }
    return path_length == volume_length && _wcsnicmp(path, volume, path_length) == 0;
}

/* Process identity, profile validation and detached PowerShell uninstall execution. */
void system_set_restrictive_umask(void) {
    _umask(0077);
}

CupError system_get_home_dir(char *buffer, size_t size) {
    wchar_t value[MAX_PATH_LEN];
    wchar_t absolute[MAX_PATH_LEN];
    DWORD length;
    size_t i;

    if (buffer == NULL || size == 0) {
        return CUP_ERR_INVALID_INPUT;
    }

    length = GetEnvironmentVariableW(L"USERPROFILE", value, MAX_PATH_LEN);
    if (length == 0 || length >= MAX_PATH_LEN) {
        print_windows_error("could not read USERPROFILE", NULL);
        return CUP_ERR_FILESYSTEM;
    }
    for (i = 0; value[i] != L'\0'; ++i) {
        if (value[i] == L'/') {
            value[i] = L'\\';
        }
    }
    if (!wide_path_is_absolute(value)) {
        fprintf(stderr, "Error: USERPROFILE must contain an absolute path.\n");
        return CUP_ERR_FILESYSTEM;
    }
    length = GetFullPathNameW(value, MAX_PATH_LEN, absolute, NULL);
    if (length == 0 || length >= MAX_PATH_LEN || wide_path_is_volume_root(absolute)) {
        fprintf(stderr,
                "Error: USERPROFILE must be an absolute user directory, not a volume root.\n");
        return CUP_ERR_FILESYSTEM;
    }

    {
        CupError err = wide_to_utf8(absolute, buffer, size);

        return err == CUP_OK ? path_normalize(buffer) : err;
    }
}

unsigned long system_get_process_id(void) {
    return (unsigned long)GetCurrentProcessId();
}

CupError system_start_uninstall(const char *cup_root,
                                const char *uninstall_script,
                                unsigned long parent_pid) {
    wchar_t temp_directory_wide[MAX_PATH_LEN];
    wchar_t temp_script_wide[MAX_PATH_LEN];
    wchar_t wide_root[MAX_PATH_LEN];
    wchar_t wide_command[MAX_PATH_LEN * 4];
    char temp_directory[MAX_PATH_LEN];
    char temp_script[MAX_PATH_LEN];
    FILE *file = NULL;
    STARTUPINFOEXW startup;
    PROCESS_INFORMATION process;
    SECURITY_ATTRIBUTES pipe_security;
    HANDLE parent_handle = NULL;
    HANDLE ready_read = NULL;
    HANDLE ready_write = NULL;
    HANDLE inherited_handles[2];
    SIZE_T attribute_size = 0;
    DWORD length;
    DWORD process_error = ERROR_SUCCESS;
    DWORD acknowledgement_size = 0;
    char acknowledgement = '\0';
    int attributes_initialized = 0;
    int written;
    CupError err = CUP_ERR_FILESYSTEM;

    ZeroMemory(&startup, sizeof(startup));
    ZeroMemory(&process, sizeof(process));

    if (text_is_empty(cup_root) || text_is_empty(uninstall_script) ||
        parent_pid != system_get_process_id()) {
        return CUP_ERR_INVALID_INPUT;
    }

    length = GetTempPathW(MAX_PATH_LEN, temp_directory_wide);
    if (length == 0 || length >= MAX_PATH_LEN) {
        print_windows_error("could not read the temporary directory", NULL);
        return CUP_ERR_FILESYSTEM;
    }
    if (wide_to_utf8(temp_directory_wide, temp_directory, sizeof(temp_directory)) != CUP_OK ||
        path_normalize(temp_directory) != CUP_OK ||
        create_temp_file_with_suffix(
            temp_directory, "cup-uninstall", ".ps1", temp_script, sizeof(temp_script), &file) !=
            CUP_OK) {
        return CUP_ERR_FILESYSTEM;
    }
    if (fclose(file) != 0) {
        system_remove_file(temp_script);
        return CUP_ERR_FILESYSTEM;
    }
    file = NULL;
    if (system_copy_file(uninstall_script, temp_script) != CUP_OK ||
        utf8_to_wide_process_path(temp_script, temp_script_wide, MAX_PATH_LEN) != CUP_OK ||
        utf8_to_wide_process_path(cup_root, wide_root, MAX_PATH_LEN) != CUP_OK) {
        system_remove_file(temp_script);
        return CUP_ERR_FILESYSTEM;
    }

    if (!DuplicateHandle(GetCurrentProcess(),
                         GetCurrentProcess(),
                         GetCurrentProcess(),
                         &parent_handle,
                         SYNCHRONIZE,
                         TRUE,
                         0)) {
        process_error = GetLastError();
        goto cleanup;
    }

    ZeroMemory(&pipe_security, sizeof(pipe_security));
    pipe_security.nLength = sizeof(pipe_security);
    pipe_security.bInheritHandle = TRUE;
    if (!CreatePipe(&ready_read, &ready_write, &pipe_security, 0) ||
        !SetHandleInformation(ready_read, HANDLE_FLAG_INHERIT, 0)) {
        process_error = GetLastError();
        goto cleanup;
    }

    written = _snwprintf(wide_command,
                         sizeof(wide_command) / sizeof(wide_command[0]),
                         L"powershell.exe -NoProfile -ExecutionPolicy Bypass "
                         L"-File \"%ls\" -CupRoot \"%ls\" -SelfPath \"%ls\" "
                         L"-ParentPid %lu -ParentHandle %llu -ReadyHandle %llu",
                         temp_script_wide,
                         wide_root,
                         temp_script_wide,
                         parent_pid,
                         (unsigned long long)(uintptr_t)parent_handle,
                         (unsigned long long)(uintptr_t)ready_write);
    if (written < 0 || (size_t)written >= sizeof(wide_command) / sizeof(wide_command[0])) {
        err = CUP_ERR_BUFFER_TOO_SMALL;
        goto cleanup;
    }

    startup.StartupInfo.cb = sizeof(startup);
    InitializeProcThreadAttributeList(NULL, 1, 0, &attribute_size);
    if (attribute_size == 0) {
        process_error = GetLastError();
        goto cleanup;
    }
    startup.lpAttributeList = HeapAlloc(GetProcessHeap(), 0, attribute_size);
    if (startup.lpAttributeList == NULL) {
        process_error = ERROR_NOT_ENOUGH_MEMORY;
        goto cleanup;
    }
    if (!InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &attribute_size)) {
        process_error = GetLastError();
        goto cleanup;
    }
    attributes_initialized = 1;
    inherited_handles[0] = parent_handle;
    inherited_handles[1] = ready_write;
    if (!UpdateProcThreadAttribute(startup.lpAttributeList,
                                   0,
                                   PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                   inherited_handles,
                                   sizeof(inherited_handles),
                                   NULL,
                                   NULL)) {
        process_error = GetLastError();
        goto cleanup;
    }

    if (!CreateProcessW(NULL,
                        wide_command,
                        NULL,
                        NULL,
                        TRUE,
                        CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP | EXTENDED_STARTUPINFO_PRESENT,
                        NULL,
                        temp_directory_wide,
                        &startup.StartupInfo,
                        &process)) {
        process_error = GetLastError();
        goto cleanup;
    }

    CloseHandle(process.hThread);
    process.hThread = NULL;
    CloseHandle(ready_write);
    ready_write = NULL;
    if (!ReadFile(ready_read, &acknowledgement, 1, &acknowledgement_size, NULL)) {
        process_error = GetLastError();
        goto cleanup;
    }
    if (acknowledgement_size != 1 || acknowledgement != 'R') {
        process_error = ERROR_INVALID_DATA;
        goto cleanup;
    }

    CloseHandle(process.hProcess);
    process.hProcess = NULL;
    err = CUP_OK;

cleanup:
    if (process.hThread != NULL) {
        CloseHandle(process.hThread);
    }
    if (process.hProcess != NULL) {
        WaitForSingleObject(process.hProcess, 5000);
        CloseHandle(process.hProcess);
    }
    if (ready_read != NULL) {
        CloseHandle(ready_read);
    }
    if (ready_write != NULL) {
        CloseHandle(ready_write);
    }
    if (parent_handle != NULL) {
        CloseHandle(parent_handle);
    }
    if (attributes_initialized) {
        DeleteProcThreadAttributeList(startup.lpAttributeList);
    }
    if (startup.lpAttributeList != NULL) {
        HeapFree(GetProcessHeap(), 0, startup.lpAttributeList);
    }
    if (err != CUP_OK) {
        if (process_error != ERROR_SUCCESS) {
            SetLastError(process_error);
            print_windows_error("could not start uninstall process", temp_script);
        }
        system_remove_file(temp_script);
    }
    return err;
}

/* Wide-API creation, copy, replacement and recursive mutation with reparse-point checks. */
CupError system_make_directory(const char *path) {
    wchar_t wide_path[MAX_PATH_LEN];
    SystemPathKind info;

    if (utf8_to_wide_path(path, wide_path, MAX_PATH_LEN) != CUP_OK) {
        return CUP_ERR_INVALID_INPUT;
    }
    if (CreateDirectoryW(wide_path, NULL)) {
        return CUP_OK;
    }
    if (GetLastError() != ERROR_ALREADY_EXISTS || system_get_path_kind(path, &info) != CUP_OK ||
        info != SYSTEM_PATH_DIRECTORY) {
        print_windows_error("could not create directory", path);
        return CUP_ERR_FILESYSTEM;
    }
    return CUP_OK;
}

CupError system_directory_is_private(const char *path, int *is_private) {
    wchar_t wide_path[MAX_PATH_LEN];
    PSECURITY_DESCRIPTOR descriptor = NULL;
    PSID owner = NULL;
    PACL dacl = NULL;
    TOKEN_USER *user = NULL;
    DWORD status;
    DWORD i;
    SECURITY_DESCRIPTOR_CONTROL control;
    DWORD revision;
    CupError err;

    if (text_is_empty(path) || is_private == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    *is_private = 0;
    if (utf8_to_wide_path(path, wide_path, MAX_PATH_LEN) != CUP_OK) {
        return CUP_ERR_INVALID_INPUT;
    }
    err = load_current_user(&user);
    if (err != CUP_OK) {
        return err;
    }
    status = GetNamedSecurityInfoW(wide_path,
                                   SE_FILE_OBJECT,
                                   OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                                   &owner,
                                   NULL,
                                   &dacl,
                                   NULL,
                                   &descriptor);
    if (status != ERROR_SUCCESS) {
        free(user);
        return status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND
                   ? CUP_OK
                   : CUP_ERR_FILESYSTEM;
    }
    if (owner == NULL || !EqualSid(owner, user->User.Sid) || dacl == NULL ||
        !GetSecurityDescriptorControl(descriptor, &control, &revision) ||
        (control & SE_DACL_PROTECTED) == 0) {
        LocalFree(descriptor);
        free(user);
        return CUP_OK;
    }

    *is_private = 1;
    for (i = 0; i < dacl->AceCount; ++i) {
        ACE_HEADER *header = NULL;

        if (!GetAce(dacl, i, (void **)&header)) {
            *is_private = 0;
            break;
        }
        if (header->AceType == ACCESS_ALLOWED_ACE_TYPE) {
            ACCESS_ALLOWED_ACE *ace = (ACCESS_ALLOWED_ACE *)header;
            DWORD required_inheritance = OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE;
            PSID sid = (PSID)&ace->SidStart;

            /* Private root permissions must propagate to every managed descendant. */
            if (ace->Mask != 0 &&
                (!sid_is_private_principal(sid, user->User.Sid) ||
                 (header->AceFlags & required_inheritance) != required_inheritance)) {
                *is_private = 0;
                break;
            }
        } else if (header->AceType == ACCESS_ALLOWED_OBJECT_ACE_TYPE) {
            *is_private = 0;
            break;
        }
    }

    LocalFree(descriptor);
    free(user);
    return CUP_OK;
}

CupError system_make_private_directory(const char *path) {
    wchar_t wide_path[MAX_PATH_LEN];
    PSECURITY_DESCRIPTOR descriptor = NULL;
    PACL dacl = NULL;
    PSID owner = NULL;
    BOOL owner_defaulted;
    BOOL dacl_present;
    BOOL dacl_defaulted;
    SECURITY_ATTRIBUTES attributes;
    SystemPathKind kind;
    CupError err;
    int is_private;

    if (text_is_empty(path) || utf8_to_wide_path(path, wide_path, MAX_PATH_LEN) != CUP_OK) {
        return CUP_ERR_INVALID_INPUT;
    }
    err = build_private_security_descriptor(&descriptor);
    if (err != CUP_OK) {
        return err;
    }
    memset(&attributes, 0, sizeof(attributes));
    attributes.nLength = sizeof(attributes);
    attributes.lpSecurityDescriptor = descriptor;

    if (!CreateDirectoryW(wide_path, &attributes)) {
        if (GetLastError() != ERROR_ALREADY_EXISTS || system_get_path_kind(path, &kind) != CUP_OK ||
            kind != SYSTEM_PATH_DIRECTORY ||
            !GetSecurityDescriptorOwner(descriptor, &owner, &owner_defaulted) || owner == NULL ||
            !GetSecurityDescriptorDacl(descriptor, &dacl_present, &dacl, &dacl_defaulted) ||
            !dacl_present || dacl == NULL ||
            SetNamedSecurityInfoW(wide_path,
                                  SE_FILE_OBJECT,
                                  OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION |
                                      PROTECTED_DACL_SECURITY_INFORMATION,
                                  owner,
                                  NULL,
                                  dacl,
                                  NULL) != ERROR_SUCCESS) {
            LocalFree(descriptor);
            print_windows_error("could not create or secure private directory", path);
            return CUP_ERR_FILESYSTEM;
        }
    }
    LocalFree(descriptor);

    err = system_directory_is_private(path, &is_private);
    if (err != CUP_OK || !is_private) {
        fprintf(stderr, "Error: could not verify private directory '%s'.\n", path);
        return CUP_ERR_FILESYSTEM;
    }
    return CUP_OK;
}

CupError system_remove_directory(const char *path) {
    wchar_t wide_path[MAX_PATH_LEN];
    SystemPathKind info;

    if (utf8_to_wide_path(path, wide_path, MAX_PATH_LEN) != CUP_OK) {
        return CUP_ERR_INVALID_INPUT;
    }
    if (system_get_path_kind(path, &info) != CUP_OK) {
        return CUP_ERR_FILESYSTEM;
    }
    if (info == SYSTEM_PATH_MISSING) {
        return CUP_OK;
    }
    if (info != SYSTEM_PATH_DIRECTORY) {
        return CUP_ERR_FILESYSTEM;
    }
    if (!RemoveDirectoryW(wide_path)) {
        print_windows_error("could not remove directory", path);
        return CUP_ERR_FILESYSTEM;
    }
    return CUP_OK;
}

static CupError move_path_with_flags(const char *source,
                                     const char *destination,
                                     DWORD flags,
                                     SystemCommitState *commit_state) {
    wchar_t wide_source[MAX_PATH_LEN];
    wchar_t wide_destination[MAX_PATH_LEN];

    if (commit_state == NULL || utf8_to_wide_path(source, wide_source, MAX_PATH_LEN) != CUP_OK ||
        utf8_to_wide_path(destination, wide_destination, MAX_PATH_LEN) != CUP_OK) {
        return CUP_ERR_INVALID_INPUT;
    }
    *commit_state = SYSTEM_COMMIT_NOT_APPLIED;

    if (!MoveFileExW(wide_source, wide_destination, flags | MOVEFILE_WRITE_THROUGH)) {
        print_windows_error("could not move path", source);
        return CUP_ERR_FILESYSTEM;
    }

    *commit_state = SYSTEM_COMMIT_DURABLE;
    return CUP_OK;
}

CupError system_move_path(const char *source,
                          const char *destination,
                          SystemCommitState *commit_state) {
    return move_path_with_flags(source, destination, 0, commit_state);
}

CupError system_replace_file(const char *source,
                             const char *destination,
                             SystemCommitState *commit_state) {
    return move_path_with_flags(source, destination, MOVEFILE_REPLACE_EXISTING, commit_state);
}

CupError system_remove_file(const char *path) {
    wchar_t wide_path[MAX_PATH_LEN];
    DWORD attributes;

    if (utf8_to_wide_path(path, wide_path, MAX_PATH_LEN) != CUP_OK) {
        return CUP_ERR_INVALID_INPUT;
    }

    attributes = GetFileAttributesW(wide_path);
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
            return CUP_OK;
        }
        return CUP_ERR_FILESYSTEM;
    }

    if ((attributes & FILE_ATTRIBUTE_READONLY) != 0 &&
        !SetFileAttributesW(wide_path, attributes & ~FILE_ATTRIBUTE_READONLY)) {
        return CUP_ERR_FILESYSTEM;
    }

    if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 &&
        (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        return RemoveDirectoryW(wide_path) ? CUP_OK : CUP_ERR_FILESYSTEM;
    }
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        return CUP_ERR_FILESYSTEM;
    }

    if (!DeleteFileW(wide_path)) {
        print_windows_error("could not remove file", path);
        return CUP_ERR_FILESYSTEM;
    }
    return CUP_OK;
}

CupError system_copy_file(const char *source_path, const char *destination_path) {
    SystemCommitState commit_state = SYSTEM_COMMIT_NOT_APPLIED;
    wchar_t source[MAX_PATH_LEN];
    wchar_t temporary_wide[MAX_PATH_LEN];
    SystemPathKind source_info;
    FILE *temporary_file = NULL;
    CupError err;
    char parent[MAX_PATH_LEN];
    char temporary[MAX_PATH_LEN] = "";

    if (text_is_empty(source_path) || text_is_empty(destination_path)) {
        return CUP_ERR_INVALID_INPUT;
    }
    if (utf8_to_wide_path(source_path, source, MAX_PATH_LEN) != CUP_OK ||
        system_get_path_kind(source_path, &source_info) != CUP_OK ||
        source_info != SYSTEM_PATH_REGULAR_FILE) {
        return CUP_ERR_FILESYSTEM;
    }
    if (path_parent(parent, sizeof(parent), destination_path) != CUP_OK ||
        system_create_temp_file(parent, "copy", temporary, sizeof(temporary), &temporary_file) !=
            CUP_OK) {
        return CUP_ERR_FILESYSTEM;
    }

    if (fclose(temporary_file) != 0 ||
        utf8_to_wide_path(temporary, temporary_wide, MAX_PATH_LEN) != CUP_OK) {
        system_remove_file(temporary);
        return CUP_ERR_FILESYSTEM;
    }
    if (!CopyFileW(source, temporary_wide, FALSE)) {
        print_windows_error("could not copy file", source_path);
        system_remove_file(temporary);
        return CUP_ERR_FILESYSTEM;
    }

    err = system_replace_file(temporary, destination_path, &commit_state);
    if (err != CUP_OK && commit_state == SYSTEM_COMMIT_NOT_APPLIED) {
        system_remove_file(temporary);
    }
    return commit_state == SYSTEM_COMMIT_APPLIED ? CUP_ERR_COMMIT : err;
}

CupError system_sync_file(FILE *file) {
    if (file == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    if (fflush(file) != 0 || _commit(_fileno(file)) != 0) {
        return CUP_ERR_FILESYSTEM;
    }
    return CUP_OK;
}

CupError system_sync_parent_directory(const char *path) {
    if (text_is_empty(path)) {
        return CUP_ERR_INVALID_INPUT;
    }
    return CUP_OK;
}

/* Create-exclusive long-path-aware temporary files and directories. */
CupError system_create_file_exclusive(const char *path, FILE **file) {
    wchar_t wide_path[MAX_PATH_LEN];
    HANDLE handle;
    int descriptor;

    if (file == NULL || utf8_to_wide_path(path, wide_path, MAX_PATH_LEN) != CUP_OK) {
        return CUP_ERR_INVALID_INPUT;
    }
    *file = NULL;

    handle = CreateFileW(
        wide_path, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (handle == INVALID_HANDLE_VALUE) {
        DWORD error = GetLastError();
        return error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS ? CUP_ERR_LOCK
                                                                           : CUP_ERR_FILESYSTEM;
    }

    descriptor = _open_osfhandle((intptr_t)handle, _O_BINARY | _O_RDWR);
    if (descriptor == -1) {
        CloseHandle(handle);
        system_remove_file(path);
        return CUP_ERR_FILESYSTEM;
    }
    *file = _fdopen(descriptor, "w+b");
    if (*file == NULL) {
        _close(descriptor);
        system_remove_file(path);
        return CUP_ERR_FILESYSTEM;
    }
    return CUP_OK;
}

CupError system_create_temp_file(
    const char *directory, const char *prefix, char *path, size_t path_size, FILE **file) {
    HANDLE handle;
    int descriptor;

    if (file == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    *file = NULL;

    if (open_temp_handle(directory, prefix, ".tmp", path, path_size, &handle) != CUP_OK) {
        return CUP_ERR_TEMPORARY;
    }

    descriptor = _open_osfhandle((intptr_t)handle, _O_BINARY | _O_RDWR);
    if (descriptor == -1) {
        CloseHandle(handle);
        system_remove_file(path);
        return CUP_ERR_TEMPORARY;
    }

    *file = _fdopen(descriptor, "w+b");
    if (*file == NULL) {
        _close(descriptor);
        system_remove_file(path);
        return CUP_ERR_TEMPORARY;
    }

    return CUP_OK;
}

CupError system_create_temp_directory(const char *directory,
                                      const char *prefix,
                                      char *path,
                                      size_t path_size) {
    unsigned long attempt;

    if (text_is_empty(directory) || text_is_empty(prefix) || path == NULL || path_size == 0) {
        return CUP_ERR_INVALID_INPUT;
    }

    for (attempt = 0; attempt < 256; ++attempt) {
        wchar_t wide_path[MAX_PATH_LEN];

        if (build_temp_candidate(directory, prefix, ".tmp", attempt, path, path_size) != CUP_OK ||
            utf8_to_wide_path(path, wide_path, MAX_PATH_LEN) != CUP_OK) {
            return CUP_ERR_TEMPORARY;
        }
        if (CreateDirectoryW(wide_path, NULL)) {
            return CUP_OK;
        }
        if (GetLastError() != ERROR_FILE_EXISTS && GetLastError() != ERROR_ALREADY_EXISTS) {
            return CUP_ERR_TEMPORARY;
        }
    }

    return CUP_ERR_TEMPORARY;
}

CupError system_make_unique_temp_path(const char *directory,
                                      const char *prefix,
                                      char *path,
                                      size_t path_size) {
    FILE *file = NULL;

    if (system_create_temp_file(directory, prefix, path, path_size, &file) != CUP_OK) {
        return CUP_ERR_TEMPORARY;
    }
    {
        int close_failed = fclose(file) != 0;
        int remove_failed = system_remove_file(path) != CUP_OK;

        return close_failed || remove_failed ? CUP_ERR_TEMPORARY : CUP_OK;
    }
}

/* Inspect path type without traversing reparse points and expose native attributes safely. */
CupError system_get_path_kind(const char *path, SystemPathKind *path_kind) {
    wchar_t wide_path[MAX_PATH_LEN];
    DWORD attributes;

    if (path_kind == NULL || utf8_to_wide_path(path, wide_path, MAX_PATH_LEN) != CUP_OK) {
        return CUP_ERR_INVALID_INPUT;
    }

    *path_kind = SYSTEM_PATH_MISSING;
    attributes = GetFileAttributesW(wide_path);
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        DWORD error = GetLastError();

        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ||
            error == ERROR_INVALID_NAME) {
            return CUP_OK;
        }
        return CUP_ERR_FILESYSTEM;
    }

    *path_kind = path_kind_from_attributes(attributes);
    return CUP_OK;
}


CupError system_file_size(const char *path, long long *file_size) {
    wchar_t wide_path[MAX_PATH_LEN];
    HANDLE file;
    LARGE_INTEGER value;
    SystemPathKind info;

    CupError err;

    if (file_size == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    *file_size = 0;
    if (text_is_empty(path)) {
        return CUP_ERR_INVALID_INPUT;
    }
    err = utf8_to_wide_path(path, wide_path, MAX_PATH_LEN);
    if (err != CUP_OK) {
        return err;
    }
    err = system_get_path_kind(path, &info);
    if (err != CUP_OK) {
        return err;
    }
    if (info != SYSTEM_PATH_REGULAR_FILE) {
        return CUP_ERR_FILESYSTEM;
    }
    file = CreateFileW(wide_path,
                       GENERIC_READ,
                       FILE_SHARE_READ,
                       NULL,
                       OPEN_EXISTING,
                       FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
                       NULL);
    if (file == INVALID_HANDLE_VALUE) {
        return CUP_ERR_FILESYSTEM;
    }
    if (!GetFileSizeEx(file, &value)) {
        CloseHandle(file);
        return CUP_ERR_FILESYSTEM;
    }
    CloseHandle(file);
    *file_size = (long long)value.QuadPart;
    return CUP_OK;
}

/* Private DACL creation plus executable/read-only compatibility controls. */
CupError system_is_executable(const char *path, int *is_executable) {
    SystemPathKind info;
    CupError err;

    if (is_executable == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    *is_executable = 0;
    if (text_is_empty(path)) {
        return CUP_ERR_INVALID_INPUT;
    }
    err = system_get_path_kind(path, &info);
    if (err != CUP_OK) {
        return err;
    }
    *is_executable = info == SYSTEM_PATH_REGULAR_FILE && has_command_extension(path);
    return CUP_OK;
}

CupError system_is_read_only(const char *path, int *is_read_only) {
    wchar_t wide_path[MAX_PATH_LEN];
    DWORD attributes;
    SystemPathKind info;

    CupError err;

    if (is_read_only == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    *is_read_only = 0;
    if (text_is_empty(path)) {
        return CUP_ERR_INVALID_INPUT;
    }
    err = utf8_to_wide_path(path, wide_path, MAX_PATH_LEN);
    if (err != CUP_OK) {
        return err;
    }
    err = system_get_path_kind(path, &info);
    if (err != CUP_OK) {
        return err;
    }
    if (info != SYSTEM_PATH_REGULAR_FILE && info != SYSTEM_PATH_DIRECTORY) {
        return CUP_ERR_FILESYSTEM;
    }
    attributes = GetFileAttributesW(wide_path);
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        return CUP_ERR_FILESYSTEM;
    }
    *is_read_only = (attributes & FILE_ATTRIBUTE_READONLY) != 0;
    return CUP_OK;
}

CupError system_set_read_only(const char *path, int read_only) {
    wchar_t wide_path[MAX_PATH_LEN];
    DWORD attributes;
    SystemPathKind info;
    CupError err;

    if (text_is_empty(path)) {
        return CUP_ERR_INVALID_INPUT;
    }
    err = utf8_to_wide_path(path, wide_path, MAX_PATH_LEN);
    if (err != CUP_OK) {
        return err;
    }
    err = system_get_path_kind(path, &info);
    if (err != CUP_OK) {
        return err;
    }
    if (info != SYSTEM_PATH_REGULAR_FILE && info != SYSTEM_PATH_DIRECTORY) {
        return CUP_ERR_FILESYSTEM;
    }
    attributes = GetFileAttributesW(wide_path);
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        return CUP_ERR_FILESYSTEM;
    }
    if (read_only) {
        attributes |= FILE_ATTRIBUTE_READONLY;
    } else {
        attributes &= ~FILE_ATTRIBUTE_READONLY;
    }
    return SetFileAttributesW(wide_path, attributes) ? CUP_OK : CUP_ERR_FILESYSTEM;
}

CupError system_set_executable(const char *path, int executable) {
    SystemPathKind info;
    CupError err;

    if (text_is_empty(path)) {
        return CUP_ERR_INVALID_INPUT;
    }
    err = system_get_path_kind(path, &info);
    if (err != CUP_OK || info != SYSTEM_PATH_REGULAR_FILE) {
        return CUP_ERR_FILESYSTEM;
    }
    if (executable && !has_command_extension(path)) {
        return CUP_ERR_FILESYSTEM;
    }
    return CUP_OK;
}

/* Wide-API child enumeration with long-path normalization and reparse-point classification. */
CupError system_list_directory(const char *path, SystemDirectoryCallback callback, void *userdata) {
    wchar_t wide_path[MAX_PATH_LEN];
    wchar_t pattern[MAX_PATH_LEN];
    WIN32_FIND_DATAW data;
    HANDLE handle;
    CupError err;
    SystemPathKind root_info;

    if (callback == NULL || utf8_to_wide_path(path, wide_path, MAX_PATH_LEN) != CUP_OK) {
        return CUP_ERR_INVALID_INPUT;
    }
    err = system_get_path_kind(path, &root_info);
    if (err != CUP_OK) {
        return err;
    }
    if (root_info == SYSTEM_PATH_MISSING) {
        return CUP_OK;
    }
    if (root_info != SYSTEM_PATH_DIRECTORY) {
        return CUP_ERR_FILESYSTEM;
    }
    if (_snwprintf(pattern, MAX_PATH_LEN, L"%ls\\*", wide_path) < 0) {
        return CUP_ERR_BUFFER_TOO_SMALL;
    }

    handle = FindFirstFileW(pattern, &data);
    if (handle == INVALID_HANDLE_VALUE) {
        DWORD error = GetLastError();
        return error == ERROR_FILE_NOT_FOUND ? CUP_OK : CUP_ERR_FILESYSTEM;
    }

    do {
        char name[MAX_PATH_LEN];
        char child[MAX_PATH_LEN];
        SystemPathKind info;

        if (wcscmp(data.cFileName, L".") == 0 || wcscmp(data.cFileName, L"..") == 0) {
            continue;
        }
        if (wide_to_utf8(data.cFileName, name, sizeof(name)) != CUP_OK ||
            path_join(child, sizeof(child), path, name) != CUP_OK) {
            FindClose(handle);
            return CUP_ERR_FILESYSTEM;
        }
        info = path_kind_from_attributes(data.dwFileAttributes);
        err = callback(child, info, userdata);
        if (err != CUP_OK) {
            FindClose(handle);
            return err;
        }
    } while (FindNextFileW(handle, &data));

    if (GetLastError() != ERROR_NO_MORE_FILES) {
        FindClose(handle);
        return CUP_ERR_FILESYSTEM;
    }
    return FindClose(handle) ? CUP_OK : CUP_ERR_FILESYSTEM;
}


/* Nonblocking file locks backed by a process-owned Windows handle. */
typedef struct {
    int (*cancelled)(void);
} RemoveTreeContext;

static CupError remove_tree_callback(const char *path, SystemPathKind kind, void *userdata) {
    RemoveTreeContext *context = userdata;

    if (context != NULL && context->cancelled != NULL && context->cancelled()) {
        return CUP_ERR_INTERRUPT;
    }
    if (kind == SYSTEM_PATH_DIRECTORY) {
        return system_remove_directory(path);
    }
    return system_remove_file(path);
}

CupError system_remove_tree(const char *path, int (*cancelled)(void)) {
    SystemPathKind kind;
    RemoveTreeContext context;
    CupError err;

    if (text_is_empty(path)) {
        return CUP_ERR_INVALID_INPUT;
    }
    if (cancelled != NULL && cancelled()) {
        return CUP_ERR_INTERRUPT;
    }
    err = system_get_path_kind(path, &kind);
    if (err != CUP_OK || kind == SYSTEM_PATH_MISSING) {
        return err;
    }
    if (kind != SYSTEM_PATH_DIRECTORY) {
        return system_remove_file(path);
    }
    context.cancelled = cancelled;
    err = system_walk_directory(path, remove_tree_callback, &context);
    if (err != CUP_OK) {
        return err;
    }
    return system_remove_directory(path);
}

CupError system_lock_acquire(SystemLock *lock, const char *path, SystemLockMode mode) {
    wchar_t wide_path[MAX_PATH_LEN];
    HANDLE handle;
    OVERLAPPED overlapped;
    BY_HANDLE_FILE_INFORMATION info;
    DWORD flags = LOCKFILE_FAIL_IMMEDIATELY;

    if (lock == NULL || (mode != SYSTEM_LOCK_SHARED && mode != SYSTEM_LOCK_EXCLUSIVE) ||
        utf8_to_wide_path(path, wide_path, MAX_PATH_LEN) != CUP_OK) {
        return CUP_ERR_INVALID_INPUT;
    }
    memset(lock, 0, sizeof(*lock));

    handle = CreateFileW(wide_path,
                         GENERIC_READ | GENERIC_WRITE,
                         FILE_SHARE_READ | FILE_SHARE_WRITE,
                         NULL,
                         OPEN_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
                         NULL);
    if (handle == INVALID_HANDLE_VALUE) {
        return CUP_ERR_FILESYSTEM;
    }
    if (!GetFileInformationByHandle(handle, &info) ||
        (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0) {
        CloseHandle(handle);
        return CUP_ERR_FILESYSTEM;
    }

    if (mode == SYSTEM_LOCK_EXCLUSIVE) {
        flags |= LOCKFILE_EXCLUSIVE_LOCK;
    }
    ZeroMemory(&overlapped, sizeof(overlapped));
    if (!LockFileEx(handle, flags, 0, MAXDWORD, MAXDWORD, &overlapped)) {
        DWORD error = GetLastError();
        CloseHandle(handle);
        if (error == ERROR_LOCK_VIOLATION || error == ERROR_IO_PENDING) {
            return CUP_ERR_LOCK;
        }
        return CUP_ERR_FILESYSTEM;
    }

    lock->handle = (intptr_t)handle;
    lock->active = 1;
    return CUP_OK;
}

void system_lock_release(SystemLock *lock) {
    OVERLAPPED overlapped;
    HANDLE handle;

    if (lock == NULL || !lock->active) {
        return;
    }
    handle = (HANDLE)lock->handle;
    ZeroMemory(&overlapped, sizeof(overlapped));
    UnlockFileEx(handle, 0, MAXDWORD, MAXDWORD, &overlapped);
    CloseHandle(handle);
    lock->handle = 0;
    lock->active = 0;
}
