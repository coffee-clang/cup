/*
 * Implements the complete system.h contract with native wide-character Windows APIs, including
 * locking, replacement, attributes, traversal and detached PowerShell helpers.
 */

#include "system.h"

#include "constants.h"
#include "path.h"
#include "text.h"

#include "windows_utf.h"
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

/* UTF-8 boundary conversion and native error reporting. */

static CupError wide_to_utf8(const wchar_t *input, char *output, size_t output_size) {
    int written;

    if (input == NULL || output == NULL || output_size == 0 || output_size > INT_MAX) {
        return CUP_ERR_INVALID_INPUT;
    }

    written = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, input, -1, output, (int)output_size, NULL, NULL);
    return written == 0 ? windows_text_conversion_error() : CUP_OK;
}

static int ascii_is_alpha(char value) {
    return (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z');
}

static int wide_ascii_is_alpha(wchar_t value) {
    return (value >= L'A' && value <= L'Z') || (value >= L'a' && value <= L'z');
}

static unsigned char ascii_lower(unsigned char value) {
    return value >= 'A' && value <= 'Z' ? (unsigned char)(value + ('a' - 'A')) : value;
}

static CupError process_directory_chain(const char *path, int create, int allow_missing);
static CupError validate_directory_chain(const char *path);
static CupError validate_temp_directory(const char *path);
static CupError validate_parent_directory_chain(const char *path);

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

        while (*left != '\0' && *right != '\0' && ascii_lower(*left) == ascii_lower(*right)) {
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
    {
        CupError err = validate_temp_directory(directory);

        if (err != CUP_OK) {
            return err;
        }
    }

    for (attempt = 0; attempt < 256; ++attempt) {
        wchar_t wide_path[MAX_PATH_LEN];
        CupError err = build_temp_candidate(directory, prefix, suffix, attempt, path, path_size);

        if (err != CUP_OK) {
            return err;
        }
        err = windows_utf8_to_wide_path(path, wide_path, MAX_PATH_LEN);
        if (err != CUP_OK) {
            return err;
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
    {
        CupError err = open_temp_handle(directory, prefix, suffix, path, path_size, &handle);

        if (err != CUP_OK) {
            return err;
        }
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
        return CUP_ERR_TEMPORARY;
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

    if (sid == NULL || user_sid == NULL || !IsValidSid(sid) || !IsValidSid(user_sid)) {
        return 0;
    }
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
    if (wide_ascii_is_alpha(path[0]) && path[1] == L':' &&
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
    if (length == 0) {
        print_windows_error("could not read USERPROFILE", NULL);
        return CUP_ERR_FILESYSTEM;
    }
    if (length >= MAX_PATH_LEN) {
        return CUP_ERR_BUFFER_TOO_SMALL;
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
    if (length == 0) {
        return CUP_ERR_FILESYSTEM;
    }
    if (length >= MAX_PATH_LEN) {
        return CUP_ERR_BUFFER_TOO_SMALL;
    }
    if (wide_path_is_volume_root(absolute)) {
        fprintf(stderr,
                "Error: USERPROFILE must be an absolute user directory, not a volume root.\n");
        return CUP_ERR_FILESYSTEM;
    }

    {
        CupError err = wide_to_utf8(absolute, buffer, size);

        if (err != CUP_OK) {
            return err == CUP_ERR_INVALID_INPUT ? CUP_ERR_FILESYSTEM : err;
        }
        err = path_normalize(buffer);
        return err == CUP_ERR_INVALID_INPUT ? CUP_ERR_FILESYSTEM : err;
    }
}

unsigned long system_get_process_id(void) {
    return (unsigned long)GetCurrentProcessId();
}

static HANDLE cup_update_parent_signal = NULL;

static CupError get_system_powershell_path(wchar_t *path, size_t capacity) {
    wchar_t system_directory[MAX_PATH_LEN];
    UINT length;
    int written;

    if (path == NULL || capacity == 0 || capacity > INT_MAX) {
        return CUP_ERR_INVALID_INPUT;
    }
    length = GetSystemDirectoryW(system_directory, MAX_PATH_LEN);
    if (length == 0 || length >= MAX_PATH_LEN) {
        return CUP_ERR_FILESYSTEM;
    }
    written = _snwprintf(path,
                         capacity,
                         L"%ls\\WindowsPowerShell\\v1.0\\powershell.exe",
                         system_directory);
    if (written < 0 || (size_t)written >= capacity) {
        return CUP_ERR_BUFFER_TOO_SMALL;
    }
    return CUP_OK;
}

static CupError initialize_inherited_startup(STARTUPINFOEXW *startup,
                                             const HANDLE *handles,
                                             size_t handle_count,
                                             int *attributes_initialized,
                                             DWORD *native_error) {
    SIZE_T attribute_size = 0;

    if (startup == NULL || handles == NULL || handle_count == 0 ||
        handle_count > UINT32_MAX / sizeof(handles[0]) || attributes_initialized == NULL ||
        native_error == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    ZeroMemory(startup, sizeof(*startup));
    startup->StartupInfo.cb = sizeof(*startup);
    *attributes_initialized = 0;
    InitializeProcThreadAttributeList(NULL, 1, 0, &attribute_size);
    if (attribute_size == 0) {
        *native_error = GetLastError();
        return CUP_ERR_FILESYSTEM;
    }

    startup->lpAttributeList = HeapAlloc(GetProcessHeap(), 0, attribute_size);
    if (startup->lpAttributeList == NULL) {
        *native_error = ERROR_NOT_ENOUGH_MEMORY;
        return CUP_ERR_TEMPORARY;
    }
    if (!InitializeProcThreadAttributeList(
            startup->lpAttributeList, 1, 0, &attribute_size)) {
        *native_error = GetLastError();
        return CUP_ERR_FILESYSTEM;
    }
    *attributes_initialized = 1;
    if (!UpdateProcThreadAttribute(startup->lpAttributeList,
                                   0,
                                   PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                   (void *)handles,
                                   handle_count * sizeof(handles[0]),
                                   NULL,
                                   NULL)) {
        *native_error = GetLastError();
        return CUP_ERR_FILESYSTEM;
    }
    return CUP_OK;
}

static void release_inherited_startup(STARTUPINFOEXW *startup,
                                      int attributes_initialized) {
    if (startup == NULL) {
        return;
    }
    if (attributes_initialized && startup->lpAttributeList != NULL) {
        DeleteProcThreadAttributeList(startup->lpAttributeList);
    }
    if (startup->lpAttributeList != NULL) {
        HeapFree(GetProcessHeap(), 0, startup->lpAttributeList);
        startup->lpAttributeList = NULL;
    }
}

CupError system_start_uninstall(const char *cup_root,
                                const char *uninstall_script,
                                const char *detached_root,
                                const char *lock_path) {
    wchar_t temp_directory_wide[MAX_PATH_LEN];
    wchar_t temp_script_wide[MAX_PATH_LEN];
    wchar_t wide_root[MAX_PATH_LEN];
    wchar_t wide_lock[MAX_PATH_LEN];
    wchar_t powershell_path[MAX_PATH_LEN];
    wchar_t wide_command[MAX_PATH_LEN * 4];
    char temp_directory[MAX_PATH_LEN];
    char temp_script[MAX_PATH_LEN];
    FILE *file = NULL;
    STARTUPINFOEXW startup;
    PROCESS_INFORMATION process;
    SECURITY_ATTRIBUTES pipe_security;
    SECURITY_ATTRIBUTES lease_security;
    HANDLE parent_handle = NULL;
    HANDLE ready_read = NULL;
    HANDLE ready_write = NULL;
    HANDLE lease_handle = NULL;
    HANDLE inherited_handles[3];
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
        text_is_empty(detached_root) || text_is_empty(lock_path)) {
        return CUP_ERR_INVALID_INPUT;
    }
    (void)detached_root;

    length = GetTempPathW(MAX_PATH_LEN, temp_directory_wide);
    if (length == 0 || length >= MAX_PATH_LEN) {
        print_windows_error("could not read the temporary directory", NULL);
        return CUP_ERR_FILESYSTEM;
    }
    err = wide_to_utf8(temp_directory_wide, temp_directory, sizeof(temp_directory));
    if (err == CUP_OK) {
        err = path_normalize(temp_directory);
    }
    if (err == CUP_OK) {
        err = create_temp_file_with_suffix(
            temp_directory, "cup-uninstall", ".ps1", temp_script, sizeof(temp_script), &file);
    }
    if (err != CUP_OK) {
        return err;
    }
    if (fclose(file) != 0) {
        system_remove_file(temp_script);
        return CUP_ERR_FILESYSTEM;
    }
    file = NULL;

    err = validate_parent_directory_chain(lock_path);
    if (err == CUP_OK) {
        err = windows_utf8_to_wide_path(lock_path, wide_lock, MAX_PATH_LEN);
    }
    if (err != CUP_OK) {
        (void)system_remove_file(temp_script);
        return err;
    }

    ZeroMemory(&lease_security, sizeof(lease_security));
    lease_security.nLength = sizeof(lease_security);
    lease_security.bInheritHandle = TRUE;
    /* Deny normal CUP read/write opens while still allowing delete/rename semantics needed by
     * the root detach. The lease remains exclusive with lock_acquire_common(), whose opens
     * require read/write sharing from every existing handle. */
    lease_handle = CreateFileW(wide_lock,
                               GENERIC_READ | GENERIC_WRITE,
                               FILE_SHARE_DELETE,
                               &lease_security,
                               OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
                               NULL);
    if (lease_handle == INVALID_HANDLE_VALUE) {
        DWORD lease_error = GetLastError();

        lease_handle = NULL;
        if (lease_error == ERROR_SHARING_VIOLATION || lease_error == ERROR_LOCK_VIOLATION) {
            err = CUP_ERR_LOCK;
        } else {
            process_error = lease_error;
        }
        goto cleanup;
    }
    {
        BY_HANDLE_FILE_INFORMATION lock_info;

        if (!GetFileInformationByHandle(lease_handle, &lock_info) ||
            (lock_info.dwFileAttributes &
             (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0) {
            process_error = GetLastError();
            if (process_error == ERROR_SUCCESS) {
                process_error = ERROR_INVALID_DATA;
            }
            goto cleanup;
        }
    }

    /* Do not read the validated helper until the lifetime lease excludes repair/other mutation. */
    err = system_copy_file(uninstall_script, temp_script);
    if (err == CUP_OK) {
        err = windows_utf8_to_wide_process_path(temp_script, temp_script_wide, MAX_PATH_LEN);
    }
    if (err == CUP_OK) {
        err = windows_utf8_to_wide_process_path(cup_root, wide_root, MAX_PATH_LEN);
    }
    if (err == CUP_OK) {
        err = get_system_powershell_path(powershell_path, MAX_PATH_LEN);
    }
    if (err != CUP_OK) {
        goto cleanup;
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
                         L"\"%ls\" -NoProfile -ExecutionPolicy Bypass "
                         L"-File \"%ls\" -CupRoot \"%ls\" -SelfPath \"%ls\" "
                         L"-ParentHandle %llu -ReadyHandle %llu -LeaseHandle %llu",
                         powershell_path,
                         temp_script_wide,
                         wide_root,
                         temp_script_wide,
                         (unsigned long long)(uintptr_t)parent_handle,
                         (unsigned long long)(uintptr_t)ready_write,
                         (unsigned long long)(uintptr_t)lease_handle);
    if (written < 0 || (size_t)written >= sizeof(wide_command) / sizeof(wide_command[0])) {
        err = CUP_ERR_BUFFER_TOO_SMALL;
        goto cleanup;
    }

    inherited_handles[0] = parent_handle;
    inherited_handles[1] = ready_write;
    inherited_handles[2] = lease_handle;
    err = initialize_inherited_startup(&startup,
                                       inherited_handles,
                                       sizeof(inherited_handles) / sizeof(inherited_handles[0]),
                                       &attributes_initialized,
                                       &process_error);
    if (err != CUP_OK) {
        goto cleanup;
    }

    if (!CreateProcessW(powershell_path,
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

    /* After the helper starts, a missing or invalid ready acknowledgement leaves handoff state
     * uncertain: the child may already have changed journal or filesystem evidence. */
    err = CUP_ERR_COMMIT;

    CloseHandle(process.hThread);
    process.hThread = NULL;
    CloseHandle(lease_handle);
    lease_handle = NULL;
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
    if (lease_handle != NULL) {
        CloseHandle(lease_handle);
    }
    release_inherited_startup(&startup, attributes_initialized);
    if (err != CUP_OK) {
        if (process_error != ERROR_SUCCESS) {
            SetLastError(process_error);
            print_windows_error("could not start uninstall process", temp_script);
        }
        system_remove_file(temp_script);
    }
    return err;
}

CupError system_start_cup_update_helper(const char *helper, const char *token) {
    SECURITY_ATTRIBUTES security;
    HANDLE read_handle = NULL;
    HANDLE write_handle = NULL;
    HANDLE inherited_handles[1];
    wchar_t wide_helper[MAX_PATH_LEN];
    wchar_t wide_token[MAX_PATH_LEN];
    wchar_t command[MAX_PATH_LEN * 3];
    STARTUPINFOEXW startup;
    PROCESS_INFORMATION process;
    DWORD process_error = ERROR_SUCCESS;
    int attributes_initialized = 0;
    int written;
    CupError err = CUP_ERR_FILESYSTEM;

    ZeroMemory(&security, sizeof(security));
    ZeroMemory(&startup, sizeof(startup));
    ZeroMemory(&process, sizeof(process));
    if (text_is_empty(helper) || text_is_empty(token) || cup_update_parent_signal != NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;
    if (!CreatePipe(&read_handle, &write_handle, &security, 0) ||
        !SetHandleInformation(write_handle, HANDLE_FLAG_INHERIT, 0)) {
        process_error = GetLastError();
        goto cleanup;
    }
    err = windows_utf8_to_wide_process_path(helper, wide_helper, MAX_PATH_LEN);
    if (err == CUP_OK) {
        err = windows_utf8_to_wide(token, wide_token, MAX_PATH_LEN);
    }
    if (err != CUP_OK) {
        goto cleanup;
    }
    written = _snwprintf(command,
                         sizeof(command) / sizeof(command[0]),
                         L"\"%ls\" --internal-cup-update-helper \"%ls\" %llu",
                         wide_helper,
                         wide_token,
                         (unsigned long long)(uintptr_t)read_handle);
    if (written < 0 || (size_t)written >= sizeof(command) / sizeof(command[0])) {
        err = CUP_ERR_BUFFER_TOO_SMALL;
        goto cleanup;
    }

    inherited_handles[0] = read_handle;
    err = initialize_inherited_startup(&startup,
                                       inherited_handles,
                                       sizeof(inherited_handles) / sizeof(inherited_handles[0]),
                                       &attributes_initialized,
                                       &process_error);
    if (err != CUP_OK) {
        goto cleanup;
    }
    if (!CreateProcessW(wide_helper,
                        command,
                        NULL,
                        NULL,
                        TRUE,
                        CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP |
                            EXTENDED_STARTUPINFO_PRESENT,
                        NULL,
                        NULL,
                        &startup.StartupInfo,
                        &process)) {
        process_error = GetLastError();
        err = CUP_ERR_FILESYSTEM;
        goto cleanup;
    }

    CloseHandle(process.hThread);
    process.hThread = NULL;
    CloseHandle(process.hProcess);
    process.hProcess = NULL;
    CloseHandle(read_handle);
    read_handle = NULL;
    cup_update_parent_signal = write_handle;
    write_handle = NULL;
    err = CUP_OK;

cleanup:
    if (process.hThread != NULL) {
        CloseHandle(process.hThread);
    }
    if (process.hProcess != NULL) {
        CloseHandle(process.hProcess);
    }
    if (read_handle != NULL) {
        CloseHandle(read_handle);
    }
    if (write_handle != NULL) {
        CloseHandle(write_handle);
    }
    release_inherited_startup(&startup, attributes_initialized);
    if (err != CUP_OK && process_error != ERROR_SUCCESS) {
        SetLastError(process_error);
    }
    return err;
}

static int parse_inherited_handle(const char *value, uintptr_t *parsed) {
    uintptr_t number = 0;
    size_t i;

    if (text_is_empty(value) || parsed == NULL ||
        (value[0] == '0' && value[1] != '\0')) {
        return 0;
    }
    for (i = 0; value[i] != '\0'; ++i) {
        uintptr_t digit;

        if (value[i] < '0' || value[i] > '9') {
            return 0;
        }
        digit = (uintptr_t)(value[i] - '0');
        if (number > (UINTPTR_MAX - digit) / 10u) {
            return 0;
        }
        number = number * 10u + digit;
    }
    if (number == 0) {
        return 0;
    }
    *parsed = number;
    return 1;
}

CupError system_wait_for_parent_exit(const char *wait_value) {
    uintptr_t number;
    HANDLE handle;
    char byte;
    DWORD read_count;

    if (!parse_inherited_handle(wait_value, &number)) {
        return CUP_ERR_INVALID_INPUT;
    }

    handle = (HANDLE)number;
    while (1) {
        if (!ReadFile(handle, &byte, 1, &read_count, NULL)) {
            DWORD error = GetLastError();

            (void)CloseHandle(handle);
            return error == ERROR_BROKEN_PIPE ? CUP_OK : CUP_ERR_FILESYSTEM;
        }
        if (read_count == 0) {
            (void)CloseHandle(handle);
            return CUP_OK;
        }
    }
}

CupError system_sleep_milliseconds(unsigned int milliseconds) {
    Sleep(milliseconds);
    return CUP_OK;
}

/* Wide-API creation, copy, replacement and recursive mutation with reparse-point checks. */
static CupError absolute_normalized_path(const char *path, char *output, size_t output_size) {
    wchar_t wide_path[MAX_PATH_LEN];
    CupError err;

    if (text_is_empty(path) || output == NULL || output_size == 0) {
        return CUP_ERR_INVALID_INPUT;
    }
    err = windows_utf8_to_wide_process_path(path, wide_path, MAX_PATH_LEN);
    if (err != CUP_OK) {
        return err;
    }
    err = wide_to_utf8(wide_path, output, output_size);
    return err == CUP_OK ? path_normalize(output) : err;
}

CupError system_make_directory(const char *path) {
    wchar_t wide_path[MAX_PATH_LEN];
    SystemPathKind info;
    CupError err;

    err = validate_parent_directory_chain(path);
    if (err != CUP_OK) {
        return err;
    }
    err = windows_utf8_to_wide_path(path, wide_path, MAX_PATH_LEN);
    if (err != CUP_OK) {
        return err;
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

static CupError directory_chain_root_length(const char *path, size_t *root_length) {
    const char *separator;

    if (text_is_empty(path) || root_length == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    if (ascii_is_alpha(path[0]) && path[1] == ':' && path[2] == '/') {
        *root_length = 3;
        return CUP_OK;
    }
    if (path[0] == '/' && path[1] == '/') {
        separator = strchr(path + 2, '/');
        if (separator == NULL || separator[1] == '\0') {
            return CUP_ERR_INVALID_INPUT;
        }
        separator = strchr(separator + 1, '/');
        *root_length = separator == NULL ? strlen(path) : (size_t)(separator - path);
        return CUP_OK;
    }
    return CUP_ERR_INVALID_INPUT;
}

static CupError inspect_directory_component(const char *path, SystemPathKind *kind) {
    wchar_t wide_path[MAX_PATH_LEN];
    HANDLE handle;
    BY_HANDLE_FILE_INFORMATION information;
    CupError err;

    if (kind == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    *kind = SYSTEM_PATH_MISSING;
    err = windows_utf8_to_wide_path(path, wide_path, MAX_PATH_LEN);
    if (err != CUP_OK) {
        return err;
    }
    handle = CreateFileW(wide_path,
                         FILE_READ_ATTRIBUTES,
                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         NULL,
                         OPEN_EXISTING,
                         FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                         NULL);
    if (handle == INVALID_HANDLE_VALUE) {
        DWORD error = GetLastError();

        return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND
                   ? CUP_OK
                   : CUP_ERR_FILESYSTEM;
    }
    if (!GetFileInformationByHandle(handle, &information)) {
        CloseHandle(handle);
        return CUP_ERR_FILESYSTEM;
    }
    *kind = path_kind_from_attributes(information.dwFileAttributes);
    (void)CloseHandle(handle);
    return CUP_OK;
}

static CupError process_directory_chain(const char *path, int create, int allow_missing) {
    char normalized[MAX_PATH_LEN];
    size_t root_length;
    size_t length;
    size_t index;
    SystemPathKind kind;
    CupError err;

    if ((create != 0 && create != 1) || (allow_missing != 0 && allow_missing != 1)) {
        return CUP_ERR_INVALID_INPUT;
    }
    err = absolute_normalized_path(path, normalized, sizeof(normalized));
    if (err != CUP_OK) {
        return err;
    }
    if (directory_chain_root_length(normalized, &root_length) != CUP_OK) {
        return CUP_ERR_INVALID_INPUT;
    }

    length = strlen(normalized);
    for (index = root_length; index <= length; ++index) {
        char saved;

        if (index < length && normalized[index] != '/') {
            continue;
        }
        saved = normalized[index];
        normalized[index] = '\0';

        err = inspect_directory_component(normalized, &kind);
        if (err == CUP_OK && kind == SYSTEM_PATH_MISSING && create) {
            wchar_t wide_path[MAX_PATH_LEN];
            CupError convert_err = windows_utf8_to_wide_path(normalized, wide_path, MAX_PATH_LEN);

            if (convert_err != CUP_OK) {
                err = convert_err;
            } else if (!CreateDirectoryW(wide_path, NULL) &&
                       GetLastError() != ERROR_ALREADY_EXISTS) {
                err = CUP_ERR_FILESYSTEM;
            } else {
                err = inspect_directory_component(normalized, &kind);
            }
        }
        normalized[index] = saved;

        if (err != CUP_OK) {
            return err;
        }
        if (kind == SYSTEM_PATH_MISSING) {
            return allow_missing ? CUP_OK : CUP_ERR_FILESYSTEM;
        }
        if (kind != SYSTEM_PATH_DIRECTORY) {
            return CUP_ERR_FILESYSTEM;
        }
    }
    return CUP_OK;
}

static CupError validate_directory_chain(const char *path) {
    return process_directory_chain(path, 0, 0);
}

static CupError validate_temp_directory(const char *path) {
    CupError err = validate_directory_chain(path);

    if (err != CUP_ERR_FILESYSTEM) {
        return err;
    }
    /* Missing temp owners are temporary; unsafe or wrong-kind chains remain filesystem errors. */
    err = process_directory_chain(path, 0, 1);
    return err == CUP_OK ? CUP_ERR_TEMPORARY : err;
}

static CupError validate_parent_directory_chain(const char *path) {
    char absolute[MAX_PATH_LEN];
    char parent[MAX_PATH_LEN];
    CupError err;

    err = absolute_normalized_path(path, absolute, sizeof(absolute));
    if (err != CUP_OK) {
        return err;
    }
    err = path_parent(parent, sizeof(parent), absolute);
    return err == CUP_OK ? validate_directory_chain(parent) : err;
}

/* Read-only open reports an absent parent through missing, while unsafe parent chains fail. */
static CupError validate_open_parent_directory_chain(const char *path, int *missing) {
    char absolute[MAX_PATH_LEN];
    char parent[MAX_PATH_LEN];
    CupError err;

    if (missing == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    *missing = 0;
    err = absolute_normalized_path(path, absolute, sizeof(absolute));
    if (err != CUP_OK) {
        return err;
    }
    err = path_parent(parent, sizeof(parent), absolute);
    if (err != CUP_OK) {
        return err;
    }
    err = validate_directory_chain(parent);
    if (err != CUP_ERR_FILESYSTEM) {
        return err;
    }
    err = process_directory_chain(parent, 0, 1);
    if (err == CUP_OK) {
        *missing = 1;
    }
    return err;
}

CupError system_check_directory_chain(const char *path, int allow_missing) {
    return process_directory_chain(path, 0, allow_missing);
}

CupError system_make_directory_chain(const char *path) {
    return process_directory_chain(path, 1, 0);
}

CupError system_create_directory_exclusive(const char *path,
                                           unsigned int mode,
                                           SystemCommitState *commit_state) {
    wchar_t wide_path[MAX_PATH_LEN];
    SystemPathKind kind;

    if (text_is_empty(path) || mode > 0777u || commit_state == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    *commit_state = SYSTEM_COMMIT_NOT_APPLIED;
    {
        CupError err = validate_parent_directory_chain(path);

        if (err != CUP_OK) {
            return err;
        }
        err = windows_utf8_to_wide_path(path, wide_path, MAX_PATH_LEN);
        if (err != CUP_OK) {
            return err;
        }
    }
    if (!CreateDirectoryW(wide_path, NULL)) {
        return GetLastError() == ERROR_ALREADY_EXISTS ? CUP_ERR_LOCK : CUP_ERR_FILESYSTEM;
    }

    *commit_state = SYSTEM_COMMIT_APPLIED;
    if (system_get_path_kind(path, &kind) != CUP_OK || kind != SYSTEM_PATH_DIRECTORY ||
        system_sync_parent_directory(path) != CUP_OK) {
        return CUP_ERR_COMMIT;
    }

    *commit_state = SYSTEM_COMMIT_DURABLE;
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
    SystemPathKind kind;
    CupError err;

    if (text_is_empty(path) || is_private == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    *is_private = 0;
    err = system_get_path_kind(path, &kind);
    if (err != CUP_OK || kind == SYSTEM_PATH_MISSING) {
        return err;
    }
    if (kind != SYSTEM_PATH_DIRECTORY) {
        return CUP_OK;
    }
    err = validate_directory_chain(path);
    if (err != CUP_OK) {
        return err;
    }
    err = windows_utf8_to_wide_path(path, wide_path, MAX_PATH_LEN);
    if (err != CUP_OK) {
        return err;
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
        if (header->AceType != ACCESS_ALLOWED_ACE_TYPE) {
            *is_private = 0;
            break;
        }
        {
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
        }
    }

    LocalFree(descriptor);
    free(user);
    return CUP_OK;
}

CupError system_create_private_directory(const char *path,
                                         SystemCommitState *commit_state) {
    wchar_t wide_path[MAX_PATH_LEN];
    PSECURITY_DESCRIPTOR descriptor = NULL;
    SECURITY_ATTRIBUTES attributes;
    CupError err;
    int is_private = 0;

    if (text_is_empty(path) || commit_state == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    *commit_state = SYSTEM_COMMIT_NOT_APPLIED;
    err = validate_parent_directory_chain(path);
    if (err != CUP_OK) {
        return err;
    }
    err = windows_utf8_to_wide_path(path, wide_path, MAX_PATH_LEN);
    if (err != CUP_OK) {
        return err;
    }
    err = build_private_security_descriptor(&descriptor);
    if (err != CUP_OK) {
        return err;
    }
    memset(&attributes, 0, sizeof(attributes));
    attributes.nLength = sizeof(attributes);
    attributes.lpSecurityDescriptor = descriptor;
    if (!CreateDirectoryW(wide_path, &attributes)) {
        LocalFree(descriptor);
        return CUP_ERR_FILESYSTEM;
    }
    LocalFree(descriptor);
    *commit_state = SYSTEM_COMMIT_APPLIED;
    err = system_directory_is_private(path, &is_private);
    if (err != CUP_OK || !is_private || system_sync_parent_directory(path) != CUP_OK) {
        return CUP_ERR_COMMIT;
    }
    *commit_state = SYSTEM_COMMIT_DURABLE;
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

    if (text_is_empty(path)) {
        return CUP_ERR_INVALID_INPUT;
    }
    err = validate_parent_directory_chain(path);
    if (err != CUP_OK) {
        return err;
    }
    err = windows_utf8_to_wide_path(path, wide_path, MAX_PATH_LEN);
    if (err != CUP_OK) {
        return err;
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

static CupError open_path_handle(const char *path,
                                 DWORD access,
                                 HANDLE *handle,
                                 BY_HANDLE_FILE_INFORMATION *information,
                                 int *missing) {
    wchar_t wide_path[MAX_PATH_LEN];

    if (text_is_empty(path) || handle == NULL || information == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    {
        CupError err = windows_utf8_to_wide_path(path, wide_path, MAX_PATH_LEN);

        if (err != CUP_OK) {
            return err;
        }
    }
    if (missing != NULL) {
        *missing = 0;
    }
    *handle = CreateFileW(wide_path,
                          access,
                          FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                          NULL,
                          OPEN_EXISTING,
                          FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS,
                          NULL);
    if (*handle == INVALID_HANDLE_VALUE) {
        DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
            if (missing != NULL) {
                *missing = 1;
            }
            return CUP_OK;
        }
        return CUP_ERR_FILESYSTEM;
    }
    if (!GetFileInformationByHandle(*handle, information)) {
        CloseHandle(*handle);
        *handle = INVALID_HANDLE_VALUE;
        return CUP_ERR_FILESYSTEM;
    }
    return CUP_OK;
}

static CupError identity_from_handle_information(
    HANDLE handle,
    const BY_HANDLE_FILE_INFORMATION *information,
    SystemPathIdentity *identity) {
    FILE_ID_INFO file_id;
    unsigned int i;
    int all_zero = 1;
    int all_ff = 1;

    if (handle == INVALID_HANDLE_VALUE || information == NULL || identity == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    memset(identity, 0, sizeof(*identity));
    if (!GetFileInformationByHandleEx(handle, FileIdInfo, &file_id, sizeof(file_id))) {
        return CUP_ERR_FILESYSTEM;
    }
    for (i = 0; i < sizeof(file_id.FileId.Identifier); ++i) {
        all_zero = all_zero && file_id.FileId.Identifier[i] == 0;
        all_ff = all_ff && file_id.FileId.Identifier[i] == 0xff;
    }
    if (all_ff) {
        return CUP_ERR_FILESYSTEM;
    }

    identity->volume = (uint64_t)file_id.VolumeSerialNumber;
    if (all_zero) {
        /* File systems without a 128-bit ID expose zero in FileIdInfo. Keep their legacy
         * 64-bit identity, while ReFS and other capable file systems retain the full ID. */
        identity->object = ((uint64_t)information->nFileIndexHigh << 32) |
                           (uint64_t)information->nFileIndexLow;
    } else {
        memcpy(&identity->object, file_id.FileId.Identifier, sizeof(identity->object));
        memcpy(&identity->object_high,
               file_id.FileId.Identifier + sizeof(identity->object),
               sizeof(identity->object_high));
    }
    identity->kind = path_kind_from_attributes(information->dwFileAttributes);
    identity->valid = 1;
    return CUP_OK;
}

/* Hold a directory name stable while pathname-based enumeration uses it. Omitting
 * FILE_SHARE_DELETE prevents a concurrent rename/delete from replacing the final directory
 * entry until the pin is closed. */
static CupError open_directory_pin(const char *path,
                                   HANDLE *handle,
                                   SystemPathIdentity *identity,
                                   int *missing) {
    wchar_t wide_path[MAX_PATH_LEN];
    BY_HANDLE_FILE_INFORMATION information;
    CupError err;

    if (text_is_empty(path) || handle == NULL || identity == NULL || missing == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    *handle = INVALID_HANDLE_VALUE;
    memset(identity, 0, sizeof(*identity));
    *missing = 0;

    err = windows_utf8_to_wide_path(path, wide_path, MAX_PATH_LEN);
    if (err != CUP_OK) {
        return err;
    }
    *handle = CreateFileW(wide_path,
                          FILE_READ_ATTRIBUTES,
                          FILE_SHARE_READ | FILE_SHARE_WRITE,
                          NULL,
                          OPEN_EXISTING,
                          FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS,
                          NULL);
    if (*handle == INVALID_HANDLE_VALUE) {
        DWORD error = GetLastError();

        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
            *missing = 1;
            return CUP_OK;
        }
        return CUP_ERR_FILESYSTEM;
    }
    if (!GetFileInformationByHandle(*handle, &information)) {
        CloseHandle(*handle);
        *handle = INVALID_HANDLE_VALUE;
        return CUP_ERR_FILESYSTEM;
    }
    err = identity_from_handle_information(*handle, &information, identity);
    if (err != CUP_OK || identity->kind != SYSTEM_PATH_DIRECTORY) {
        CloseHandle(*handle);
        *handle = INVALID_HANDLE_VALUE;
        memset(identity, 0, sizeof(*identity));
        return err != CUP_OK ? err : CUP_ERR_FILESYSTEM;
    }
    return CUP_OK;
}

static CupError remove_path_handle_bound(const char *path,
                                         int require_directory,
                                         const SystemPathIdentity *expected_identity) {
    HANDLE handle = INVALID_HANDLE_VALUE;
    BY_HANDLE_FILE_INFORMATION information;
    SystemPathIdentity observed_identity;
    FILE_DISPOSITION_INFO disposition;
    FILE_BASIC_INFO original_basic;
    CupError err;
    int attributes_changed = 0;
    int missing = 0;
    int is_real_directory;

    if (text_is_empty(path) ||
        (expected_identity != NULL && !expected_identity->valid)) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = validate_parent_directory_chain(path);
    if (err != CUP_OK) {
        return err;
    }
    err = open_path_handle(path,
                           DELETE | FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES,
                           &handle,
                           &information,
                           &missing);
    if (err != CUP_OK || missing) {
        return err != CUP_OK || expected_identity == NULL ? err : CUP_ERR_FILESYSTEM;
    }

    err = identity_from_handle_information(handle, &information, &observed_identity);
    if (err != CUP_OK) {
        (void)CloseHandle(handle);
        return err;
    }
    if (expected_identity != NULL &&
        !system_path_identity_equal(&observed_identity, expected_identity)) {
        CloseHandle(handle);
        return CUP_ERR_TRANSACTION;
    }

    is_real_directory = observed_identity.kind == SYSTEM_PATH_DIRECTORY;
    if (is_real_directory != require_directory) {
        CloseHandle(handle);
        return CUP_ERR_FILESYSTEM;
    }
    if ((information.dwFileAttributes & FILE_ATTRIBUTE_READONLY) != 0) {
        FILE_BASIC_INFO writable_basic;

        if (!GetFileInformationByHandleEx(
                handle, FileBasicInfo, &original_basic, sizeof(original_basic))) {
            (void)CloseHandle(handle);
            return CUP_ERR_FILESYSTEM;
        }
        writable_basic = original_basic;
        writable_basic.FileAttributes &= ~FILE_ATTRIBUTE_READONLY;
        if (!SetFileInformationByHandle(
                handle, FileBasicInfo, &writable_basic, sizeof(writable_basic))) {
            (void)CloseHandle(handle);
            return CUP_ERR_FILESYSTEM;
        }
        attributes_changed = 1;
    }

    disposition.DeleteFile = TRUE;
    if (!SetFileInformationByHandle(
            handle, FileDispositionInfo, &disposition, sizeof(disposition))) {
        if (attributes_changed) {
            (void)SetFileInformationByHandle(
                handle, FileBasicInfo, &original_basic, sizeof(original_basic));
        }
        (void)CloseHandle(handle);
        return CUP_ERR_FILESYSTEM;
    }
    /* The delete disposition is already the mutation boundary; cleanup cannot undo it. */
    (void)CloseHandle(handle);
    return CUP_OK;
}

CupError system_remove_directory(const char *path) {
    return remove_path_handle_bound(path, 1, NULL);
}

static CupError move_path_with_flags(const char *source,
                                     const char *destination,
                                     int replace,
                                     const SystemPathIdentity *expected_source,
                                     const SystemPathIdentity *expected_destination,
                                     SystemCommitState *commit_state) {
    HANDLE source_handle = INVALID_HANDLE_VALUE;
    BY_HANDLE_FILE_INFORMATION source_information;
    SystemPathIdentity source_identity;
    SystemPathIdentity current_source;
    SystemPathIdentity destination_identity;
    wchar_t wide_source[MAX_PATH_LEN];
    wchar_t wide_destination[MAX_PATH_LEN];
    CupError result;
    DWORD move_flags = MOVEFILE_WRITE_THROUGH;
    DWORD move_error = ERROR_SUCCESS;
    int report_move_error = 0;
    int missing = 0;

    if (commit_state == NULL || text_is_empty(source) || text_is_empty(destination) ||
        (expected_source != NULL &&
         (!expected_source->valid ||
          (expected_source->kind != SYSTEM_PATH_REGULAR_FILE &&
           expected_source->kind != SYSTEM_PATH_DIRECTORY))) ||
        (expected_destination != NULL && !expected_destination->valid)) {
        return CUP_ERR_INVALID_INPUT;
    }
    *commit_state = SYSTEM_COMMIT_NOT_APPLIED;

    result = validate_parent_directory_chain(source);
    if (result == CUP_OK) {
        result = validate_parent_directory_chain(destination);
    }
    if (result != CUP_OK) {
        return result;
    }
    result = windows_utf8_to_wide_path(source, wide_source, MAX_PATH_LEN);
    if (result == CUP_OK) {
        result = windows_utf8_to_wide_path(destination, wide_destination, MAX_PATH_LEN);
    }
    if (result != CUP_OK) {
        return result;
    }

    /* Keep the observed source open while proving that its pathname still names that object. */
    result = open_path_handle(source,
                              FILE_READ_ATTRIBUTES,
                              &source_handle,
                              &source_information,
                              &missing);
    if (result != CUP_OK || missing) {
        result = result == CUP_OK ? CUP_ERR_FILESYSTEM : result;
        goto cleanup;
    }
    result = identity_from_handle_information(
        source_handle, &source_information, &source_identity);
    if (result != CUP_OK) {
        goto cleanup;
    }
    if (source_identity.kind != SYSTEM_PATH_REGULAR_FILE &&
        source_identity.kind != SYSTEM_PATH_DIRECTORY) {
        result = CUP_ERR_FILESYSTEM;
        goto cleanup;
    }
    if (source_identity.kind == SYSTEM_PATH_DIRECTORY && replace) {
        result = CUP_ERR_FILESYSTEM;
        goto cleanup;
    }
    if (expected_source != NULL &&
        !system_path_identity_equal(&source_identity, expected_source)) {
        result = CUP_ERR_TRANSACTION;
        goto cleanup;
    }

    result = system_get_path_identity(source, &current_source);
    if (result != CUP_OK || !system_path_identity_equal(&source_identity, &current_source)) {
        result = CUP_ERR_INCONSISTENT_STATE;
        goto cleanup;
    }

    /* Replacement is allowed only for the exact destination observed by the caller. */
    if (expected_destination != NULL) {
        result = system_get_path_identity(destination, &destination_identity);
        if (result != CUP_OK ||
            !system_path_identity_equal(&destination_identity, expected_destination)) {
            result = CUP_ERR_TRANSACTION;
            goto cleanup;
        }
    }

    if (replace) {
        move_flags |= MOVEFILE_REPLACE_EXISTING;
    }
    if (!MoveFileExW(wide_source, wide_destination, move_flags)) {
        move_error = GetLastError();
        report_move_error = 1;
        result = CUP_ERR_FILESYSTEM;
        goto cleanup;
    }
    *commit_state = SYSTEM_COMMIT_APPLIED;

    /* A filesystem may change its file ID during rename. Refresh the identity through the same
     * still-open source handle, then prove that the destination names that exact object. */
    if (!GetFileInformationByHandle(source_handle, &source_information)) {
        result = CUP_ERR_COMMIT;
        goto cleanup;
    }
    result = identity_from_handle_information(
        source_handle, &source_information, &source_identity);
    if (result != CUP_OK) {
        result = CUP_ERR_COMMIT;
        goto cleanup;
    }
    result = system_get_path_identity(destination, &destination_identity);
    if (result != CUP_OK ||
        !system_path_identity_equal(&source_identity, &destination_identity)) {
        result = CUP_ERR_COMMIT;
        goto cleanup;
    }

    *commit_state = SYSTEM_COMMIT_DURABLE;
    result = CUP_OK;

cleanup:
    if (source_handle != INVALID_HANDLE_VALUE) {
        (void)CloseHandle(source_handle);
    }
    if (report_move_error) {
        SetLastError(move_error);
        print_windows_error("could not move path", source);
    }
    return result;
}

CupError system_move_path(const char *source,
                          const char *destination,
                          SystemCommitState *commit_state) {
    return move_path_with_flags(source, destination, 0, NULL, NULL, commit_state);
}

CupError system_move_path_if_identity(const char *source,
                                      const char *destination,
                                      const SystemPathIdentity *expected_identity,
                                      SystemCommitState *commit_state) {
    if (expected_identity == NULL || !expected_identity->valid ||
        (expected_identity->kind != SYSTEM_PATH_REGULAR_FILE &&
         expected_identity->kind != SYSTEM_PATH_DIRECTORY)) {
        return CUP_ERR_INVALID_INPUT;
    }
    return move_path_with_flags(source, destination, 0, expected_identity, NULL, commit_state);
}

CupError system_replace_file(const char *source,
                             const char *destination,
                             SystemCommitState *commit_state) {
    return move_path_with_flags(source, destination, 1, NULL, NULL, commit_state);
}

CupError system_replace_file_if_identity(const char *source,
                                         const char *destination,
                                         const SystemPathIdentity *expected_identity,
                                         SystemCommitState *commit_state) {
    if (expected_identity == NULL || !expected_identity->valid ||
        expected_identity->kind != SYSTEM_PATH_REGULAR_FILE) {
        return CUP_ERR_INVALID_INPUT;
    }
    return move_path_with_flags(source, destination, 1, NULL, expected_identity, commit_state);
}

CupError system_remove_file(const char *path) {
    return remove_path_handle_bound(path, 0, NULL);
}

CupError system_remove_file_if_identity(const char *path,
                                        const SystemPathIdentity *expected_identity) {
    if (expected_identity == NULL || !expected_identity->valid ||
        expected_identity->kind != SYSTEM_PATH_REGULAR_FILE) {
        return CUP_ERR_INVALID_INPUT;
    }
    return remove_path_handle_bound(path, 0, expected_identity);
}

CupError system_copy_file(const char *source_path, const char *destination_path) {
    SystemCommitState commit_state = SYSTEM_COMMIT_NOT_APPLIED;
    SystemPathIdentity source_identity;
    FILE *source = NULL;
    FILE *destination = NULL;
    unsigned char buffer[8192];
    uint64_t source_size = 0;
    size_t count;
    int missing = 0;
    int failed = 0;
    CupError err;
    char parent[MAX_PATH_LEN];
    char temporary[MAX_PATH_LEN] = "";

    if (text_is_empty(source_path) || text_is_empty(destination_path)) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = system_open_regular_file(
        source_path, &source, &source_identity, &source_size, &missing);
    if (err != CUP_OK || missing) {
        if (source != NULL) {
            fclose(source);
        }
        return err != CUP_OK ? err : CUP_ERR_FILESYSTEM;
    }
    (void)source_identity;
    (void)source_size;

    err = path_parent(parent, sizeof(parent), destination_path);
    if (err == CUP_OK) {
        err = system_create_temp_file(
            parent, "copy", temporary, sizeof(temporary), &destination);
    }
    if (err != CUP_OK) {
        fclose(source);
        /* The sibling temporary is a copy implementation detail: temporary-object creation
         * failures surface as filesystem errors, while capacity/input errors stay specific. */
        return err == CUP_ERR_TEMPORARY ? CUP_ERR_FILESYSTEM : err;
    }

    while (1) {
        count = fread(buffer, 1, sizeof(buffer), source);
        if (count > 0 && fwrite(buffer, 1, count, destination) != count) {
            failed = 1;
            break;
        }
        if (count < sizeof(buffer)) {
            if (ferror(source) != 0) {
                failed = 1;
            }
            break;
        }
    }
    if (fclose(source) != 0) {
        failed = 1;
    }
    source = NULL;
    if (!failed && system_sync_file(destination) != CUP_OK) {
        failed = 1;
    }
    if (fclose(destination) != 0) {
        failed = 1;
    }
    destination = NULL;

    if (failed) {
        system_remove_file(temporary);
        return CUP_ERR_FILESYSTEM;
    }

    err = system_replace_file(temporary, destination_path, &commit_state);
    if (err != CUP_OK && commit_state == SYSTEM_COMMIT_NOT_APPLIED) {
        system_remove_file(temporary);
    }
    return commit_state == SYSTEM_COMMIT_APPLIED ? CUP_ERR_COMMIT : err;
}

CupError system_set_file_executable(FILE *file, int executable) {
    if (file == NULL || (executable != 0 && executable != 1)) {
        return CUP_ERR_INVALID_INPUT;
    }

    /* Windows command executability is determined by the final filename extension. */
    return _fileno(file) >= 0 ? CUP_OK : CUP_ERR_FILESYSTEM;
}

CupError system_file_is_executable(FILE *file, const char *path, int *is_executable) {
    if (file == NULL || text_is_empty(path) || is_executable == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    *is_executable = 0;
    if (_fileno(file) < 0) {
        return CUP_ERR_FILESYSTEM;
    }
    *is_executable = has_command_extension(path);
    return CUP_OK;
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
    char parent[MAX_PATH_LEN];
    wchar_t wide_parent[MAX_PATH_LEN];
    HANDLE handle;
    BY_HANDLE_FILE_INFORMATION information;
    DWORD flush_error;
    CupError err;

    if (text_is_empty(path)) {
        return CUP_ERR_INVALID_INPUT;
    }
    err = path_parent(parent, sizeof(parent), path);
    if (err != CUP_OK) {
        return err;
    }
    err = validate_directory_chain(parent);
    if (err != CUP_OK) {
        return err;
    }
    err = windows_utf8_to_wide_path(parent, wide_parent, MAX_PATH_LEN);
    if (err != CUP_OK) {
        return err;
    }

    handle = CreateFileW(wide_parent,
                         GENERIC_WRITE | FILE_READ_ATTRIBUTES,
                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         NULL,
                         OPEN_EXISTING,
                         FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                         NULL);
    if (handle == INVALID_HANDLE_VALUE) {
        flush_error = GetLastError();
        /* Windows has no portable directory-fsync contract. Treat only capability-style
         * failures as best-effort; callers that need a stronger publication guarantee use
         * write-through replacement/move primitives for the namespace change itself. */
        if (flush_error == ERROR_ACCESS_DENIED || flush_error == ERROR_INVALID_FUNCTION ||
            flush_error == ERROR_NOT_SUPPORTED) {
            return CUP_OK;
        }
        return CUP_ERR_FILESYSTEM;
    }
    if (!GetFileInformationByHandle(handle, &information) ||
        path_kind_from_attributes(information.dwFileAttributes) != SYSTEM_PATH_DIRECTORY) {
        (void)CloseHandle(handle);
        return CUP_ERR_FILESYSTEM;
    }
    if (FlushFileBuffers(handle)) {
        (void)CloseHandle(handle);
        return CUP_OK;
    }

    flush_error = GetLastError();
    (void)CloseHandle(handle);
    if (flush_error == ERROR_ACCESS_DENIED || flush_error == ERROR_INVALID_FUNCTION ||
        flush_error == ERROR_NOT_SUPPORTED) {
        return CUP_OK;
    }
    return CUP_ERR_FILESYSTEM;
}

/* Create-exclusive long-path-aware temporary files and directories. */
CupError system_create_file_exclusive(const char *path, FILE **file) {
    wchar_t wide_path[MAX_PATH_LEN];
    HANDLE handle;
    int descriptor;
    CupError err;

    if (text_is_empty(path) || file == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    *file = NULL;
    err = validate_parent_directory_chain(path);
    if (err != CUP_OK) {
        return err;
    }
    err = windows_utf8_to_wide_path(path, wide_path, MAX_PATH_LEN);
    if (err != CUP_OK) {
        return err;
    }

    handle = CreateFileW(
        wide_path, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (handle == INVALID_HANDLE_VALUE) {
        DWORD error = GetLastError();
        return error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS ? CUP_ERR_LOCK
                                                                           : CUP_ERR_FILESYSTEM;
    }

    descriptor = _open_osfhandle((intptr_t)handle, _O_BINARY | _O_RDWR);
    if (descriptor == -1) {
        (void)CloseHandle(handle);
        (void)system_remove_file(path);
        return CUP_ERR_FILESYSTEM;
    }
    *file = _fdopen(descriptor, "w+b");
    if (*file == NULL) {
        (void)_close(descriptor);
        (void)system_remove_file(path);
        return CUP_ERR_FILESYSTEM;
    }
    return CUP_OK;
}

CupError system_create_temp_file(
    const char *directory, const char *prefix, char *path, size_t path_size, FILE **file) {
    if (!path_is_safe_segment(prefix)) {
        return CUP_ERR_INVALID_INPUT;
    }
    return create_temp_file_with_suffix(directory, prefix, ".tmp", path, path_size, file);
}

CupError system_create_temp_directory(const char *directory,
                                      const char *prefix,
                                      char *path,
                                      size_t path_size) {
    unsigned long attempt;
    CupError err;

    if (text_is_empty(directory) || !path_is_safe_segment(prefix) || path == NULL ||
        path_size == 0) {
        return CUP_ERR_INVALID_INPUT;
    }
    err = validate_temp_directory(directory);
    if (err != CUP_OK) {
        return err;
    }

    for (attempt = 0; attempt < 256; ++attempt) {
        wchar_t wide_path[MAX_PATH_LEN];

        err = build_temp_candidate(directory, prefix, ".tmp", attempt, path, path_size);
        if (err != CUP_OK) {
            return err;
        }
        err = windows_utf8_to_wide_path(path, wide_path, MAX_PATH_LEN);
        if (err != CUP_OK) {
            return err;
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
    CupError err;

    err = system_create_temp_file(directory, prefix, path, path_size, &file);
    if (err != CUP_OK) {
        return err;
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
    CupError err;

    if (path_kind == NULL || text_is_empty(path)) {
        return CUP_ERR_INVALID_INPUT;
    }
    err = windows_utf8_to_wide_path(path, wide_path, MAX_PATH_LEN);
    if (err != CUP_OK) {
        return err;
    }

    *path_kind = SYSTEM_PATH_MISSING;
    attributes = GetFileAttributesW(wide_path);
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        DWORD error = GetLastError();

        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
            return CUP_OK;
        }
        return CUP_ERR_FILESYSTEM;
    }

    *path_kind = path_kind_from_attributes(attributes);
    return CUP_OK;
}


CupError system_open_regular_file(const char *path,
                                  FILE **file,
                                  SystemPathIdentity *identity,
                                  uint64_t *file_size,
                                  int *missing) {
    wchar_t wide_path[MAX_PATH_LEN];
    HANDLE handle;
    BY_HANDLE_FILE_INFORMATION information;
    int descriptor;

    CupError err;

    if (text_is_empty(path) || file == NULL || identity == NULL || file_size == NULL ||
        missing == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    *file = NULL;
    *file_size = 0;
    *missing = 0;
    memset(identity, 0, sizeof(*identity));
    err = validate_open_parent_directory_chain(path, missing);
    if (err != CUP_OK || *missing) {
        return err;
    }
    err = windows_utf8_to_wide_path(path, wide_path, MAX_PATH_LEN);
    if (err != CUP_OK) {
        return err;
    }

    handle = CreateFileW(wide_path,
                         GENERIC_READ,
                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         NULL,
                         OPEN_EXISTING,
                         FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN,
                         NULL);
    if (handle == INVALID_HANDLE_VALUE) {
        DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
            *missing = 1;
            return CUP_OK;
        }
        return CUP_ERR_FILESYSTEM;
    }
    if (!GetFileInformationByHandle(handle, &information) ||
        (information.dwFileAttributes &
         (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0) {
        CloseHandle(handle);
        return CUP_ERR_FILESYSTEM;
    }
    err = identity_from_handle_information(handle, &information, identity);
    if (err != CUP_OK || identity->kind != SYSTEM_PATH_REGULAR_FILE) {
        (void)CloseHandle(handle);
        memset(identity, 0, sizeof(*identity));
        return err != CUP_OK ? err : CUP_ERR_FILESYSTEM;
    }
    *file_size = ((uint64_t)information.nFileSizeHigh << 32) | information.nFileSizeLow;

    descriptor = _open_osfhandle((intptr_t)handle, _O_BINARY | _O_RDONLY);
    if (descriptor == -1) {
        CloseHandle(handle);
        memset(identity, 0, sizeof(*identity));
        *file_size = 0;
        return CUP_ERR_FILESYSTEM;
    }
    *file = _fdopen(descriptor, "rb");
    if (*file == NULL) {
        _close(descriptor);
        memset(identity, 0, sizeof(*identity));
        *file_size = 0;
        return CUP_ERR_FILESYSTEM;
    }
    return CUP_OK;
}

CupError system_get_path_identity(const char *path, SystemPathIdentity *identity) {
    HANDLE handle = INVALID_HANDLE_VALUE;
    BY_HANDLE_FILE_INFORMATION information;
    CupError err;
    int missing = 0;

    if (identity == NULL || text_is_empty(path)) {
        return CUP_ERR_INVALID_INPUT;
    }
    memset(identity, 0, sizeof(*identity));
    err = open_path_handle(
        path, FILE_READ_ATTRIBUTES, &handle, &information, &missing);
    if (err != CUP_OK || missing) {
        return err;
    }
    err = identity_from_handle_information(handle, &information, identity);
    (void)CloseHandle(handle);
    return err;
}

int system_path_identity_equal(const SystemPathIdentity *left,
                               const SystemPathIdentity *right) {
    return left != NULL && right != NULL && left->valid && right->valid &&
           left->volume == right->volume && left->object == right->object &&
           left->object_high == right->object_high && left->kind == right->kind;
}

CupError system_file_size(const char *path, long long *file_size) {
    HANDLE handle = INVALID_HANDLE_VALUE;
    BY_HANDLE_FILE_INFORMATION information;
    CupError err;
    int missing = 0;

    if (file_size == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    *file_size = 0;
    if (text_is_empty(path)) {
        return CUP_ERR_INVALID_INPUT;
    }
    err = open_path_handle(
        path, FILE_READ_ATTRIBUTES, &handle, &information, &missing);
    if (err != CUP_OK || missing) {
        return err != CUP_OK ? err : CUP_ERR_FILESYSTEM;
    }
    if (path_kind_from_attributes(information.dwFileAttributes) !=
        SYSTEM_PATH_REGULAR_FILE) {
        (void)CloseHandle(handle);
        return CUP_ERR_FILESYSTEM;
    }
    {
        uint64_t size = ((uint64_t)information.nFileSizeHigh << 32) |
                        (uint64_t)information.nFileSizeLow;
        if (size > (uint64_t)LLONG_MAX) {
            (void)CloseHandle(handle);
            return CUP_ERR_FILESYSTEM;
        }
        *file_size = (long long)size;
    }
    (void)CloseHandle(handle);
    return CUP_OK;
}

/* Private DACL creation plus executable/read-only compatibility controls. */
CupError system_is_executable(const char *path, int *is_executable) {
    HANDLE handle = INVALID_HANDLE_VALUE;
    BY_HANDLE_FILE_INFORMATION information;
    CupError err;
    int missing = 0;

    if (is_executable == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    *is_executable = 0;
    if (text_is_empty(path)) {
        return CUP_ERR_INVALID_INPUT;
    }
    err = open_path_handle(
        path, FILE_READ_ATTRIBUTES, &handle, &information, &missing);
    if (err != CUP_OK || missing) {
        return err;
    }
    *is_executable =
        path_kind_from_attributes(information.dwFileAttributes) == SYSTEM_PATH_REGULAR_FILE &&
        has_command_extension(path);
    (void)CloseHandle(handle);
    return CUP_OK;
}

CupError system_is_read_only(const char *path, int *is_read_only) {
    HANDLE handle = INVALID_HANDLE_VALUE;
    BY_HANDLE_FILE_INFORMATION information;
    CupError err;
    int missing = 0;

    if (is_read_only == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    *is_read_only = 0;
    if (text_is_empty(path)) {
        return CUP_ERR_INVALID_INPUT;
    }
    err = open_path_handle(
        path, FILE_READ_ATTRIBUTES, &handle, &information, &missing);
    if (err != CUP_OK || missing) {
        return err != CUP_OK ? err : CUP_ERR_FILESYSTEM;
    }
    if (path_kind_from_attributes(information.dwFileAttributes) != SYSTEM_PATH_REGULAR_FILE &&
        path_kind_from_attributes(information.dwFileAttributes) != SYSTEM_PATH_DIRECTORY) {
        (void)CloseHandle(handle);
        return CUP_ERR_FILESYSTEM;
    }
    *is_read_only = (information.dwFileAttributes & FILE_ATTRIBUTE_READONLY) != 0;
    (void)CloseHandle(handle);
    return CUP_OK;
}

CupError system_set_read_only(const char *path, int read_only) {
    HANDLE handle = INVALID_HANDLE_VALUE;
    BY_HANDLE_FILE_INFORMATION information;
    FILE_BASIC_INFO basic;
    CupError err;
    int missing = 0;

    if (text_is_empty(path) || (read_only != 0 && read_only != 1)) {
        return CUP_ERR_INVALID_INPUT;
    }
    err = validate_parent_directory_chain(path);
    if (err != CUP_OK) {
        return err;
    }
    err = open_path_handle(path,
                           FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES,
                           &handle,
                           &information,
                           &missing);
    if (err != CUP_OK || missing) {
        return err != CUP_OK ? err : CUP_ERR_FILESYSTEM;
    }
    if (path_kind_from_attributes(information.dwFileAttributes) != SYSTEM_PATH_REGULAR_FILE &&
        path_kind_from_attributes(information.dwFileAttributes) != SYSTEM_PATH_DIRECTORY) {
        (void)CloseHandle(handle);
        return CUP_ERR_FILESYSTEM;
    }
    if (!GetFileInformationByHandleEx(handle, FileBasicInfo, &basic, sizeof(basic))) {
        CloseHandle(handle);
        return CUP_ERR_FILESYSTEM;
    }
    if (read_only) {
        basic.FileAttributes |= FILE_ATTRIBUTE_READONLY;
    } else {
        basic.FileAttributes &= ~FILE_ATTRIBUTE_READONLY;
    }
    if (!SetFileInformationByHandle(handle, FileBasicInfo, &basic, sizeof(basic))) {
        (void)CloseHandle(handle);
        return CUP_ERR_FILESYSTEM;
    }
    (void)CloseHandle(handle);
    return CUP_OK;
}

CupError system_set_executable(const char *path, int executable) {
    HANDLE handle = INVALID_HANDLE_VALUE;
    BY_HANDLE_FILE_INFORMATION information;
    CupError err;
    int missing = 0;

    if (text_is_empty(path) || (executable != 0 && executable != 1)) {
        return CUP_ERR_INVALID_INPUT;
    }
    err = validate_parent_directory_chain(path);
    if (err != CUP_OK) {
        return err;
    }
    err = open_path_handle(
        path, FILE_READ_ATTRIBUTES, &handle, &information, &missing);
    if (err != CUP_OK || missing) {
        return err != CUP_OK ? err : CUP_ERR_FILESYSTEM;
    }
    if (path_kind_from_attributes(information.dwFileAttributes) != SYSTEM_PATH_REGULAR_FILE ||
        (executable && !has_command_extension(path))) {
        (void)CloseHandle(handle);
        return CUP_ERR_FILESYSTEM;
    }
    (void)CloseHandle(handle);
    return CUP_OK;
}

/* Wide-API child enumeration with long-path normalization and reparse-point classification. */
static CupError list_directory_bound(const char *path,
                                     const SystemPathIdentity *expected_identity,
                                     SystemDirectoryCallback callback,
                                     void *userdata) {
    wchar_t wide_path[MAX_PATH_LEN];
    wchar_t pattern[MAX_PATH_LEN];
    WIN32_FIND_DATAW data;
    HANDLE directory = INVALID_HANDLE_VALUE;
    HANDLE search;
    SystemPathIdentity observed_identity;
    CupError err;
    SystemPathKind root_info;
    int missing = 0;

    if (callback == NULL || text_is_empty(path) ||
        (expected_identity != NULL && !expected_identity->valid)) {
        return CUP_ERR_INVALID_INPUT;
    }
    err = system_get_path_kind(path, &root_info);
    if (err != CUP_OK) {
        return err;
    }
    if (root_info == SYSTEM_PATH_MISSING) {
        return expected_identity == NULL ? CUP_OK : CUP_ERR_FILESYSTEM;
    }
    if (root_info != SYSTEM_PATH_DIRECTORY) {
        return CUP_ERR_FILESYSTEM;
    }
    err = validate_directory_chain(path);
    if (err != CUP_OK) {
        return err;
    }
    err = open_directory_pin(path, &directory, &observed_identity, &missing);
    if (err != CUP_OK || missing) {
        return err != CUP_OK || expected_identity == NULL ? err : CUP_ERR_FILESYSTEM;
    }
    if (expected_identity != NULL &&
        !system_path_identity_equal(&observed_identity, expected_identity)) {
        CloseHandle(directory);
        return CUP_ERR_FILESYSTEM;
    }
    err = windows_utf8_to_wide_path(path, wide_path, MAX_PATH_LEN);
    if (err != CUP_OK) {
        CloseHandle(directory);
        return err;
    }
    if (_snwprintf(pattern, MAX_PATH_LEN, L"%ls\\*", wide_path) < 0) {
        CloseHandle(directory);
        return CUP_ERR_BUFFER_TOO_SMALL;
    }

    search = FindFirstFileW(pattern, &data);
    if (search == INVALID_HANDLE_VALUE) {
        DWORD error = GetLastError();

        CloseHandle(directory);
        return error == ERROR_FILE_NOT_FOUND ? CUP_OK : CUP_ERR_FILESYSTEM;
    }

    do {
        char name[MAX_PATH_LEN];
        char child[MAX_PATH_LEN];
        SystemPathKind info;

        if (wcscmp(data.cFileName, L".") == 0 || wcscmp(data.cFileName, L"..") == 0) {
            continue;
        }
        err = wide_to_utf8(data.cFileName, name, sizeof(name));
        if (err == CUP_OK) {
            err = path_join(child, sizeof(child), path, name);
        }
        if (err != CUP_OK) {
            (void)FindClose(search);
            CloseHandle(directory);
            return err;
        }
        {
            SystemPathIdentity identity;

            info = path_kind_from_attributes(data.dwFileAttributes);
            err = system_get_path_identity(child, &identity);
            if (err != CUP_OK || !identity.valid || identity.kind != info) {
                FindClose(search);
                CloseHandle(directory);
                return CUP_ERR_FILESYSTEM;
            }
            err = callback(child, info, &identity, userdata);
        }
        if (err != CUP_OK) {
            FindClose(search);
            CloseHandle(directory);
            return err;
        }
    } while (FindNextFileW(search, &data));

    if (GetLastError() != ERROR_NO_MORE_FILES) {
        FindClose(search);
        CloseHandle(directory);
        return CUP_ERR_FILESYSTEM;
    }
    (void)FindClose(search);
    CloseHandle(directory);
    return CUP_OK;
}

CupError system_list_directory(const char *path, SystemDirectoryCallback callback, void *userdata) {
    return list_directory_bound(path, NULL, callback, userdata);
}


#define SYSTEM_MAX_TREE_DEPTH 128u

typedef struct {
    SystemDirectoryCallback callback;
    void *userdata;
    unsigned int depth;
} WindowsWalkContext;

static CupError windows_walk_entry(const char *path,
                                   SystemPathKind kind,
                                   const SystemPathIdentity *identity,
                                   void *userdata) {
    WindowsWalkContext *context = userdata;
    CupError err;

    if (context == NULL || context->callback == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    if (identity == NULL || !identity->valid || identity->kind != kind) {
        return CUP_ERR_FILESYSTEM;
    }
    if (kind == SYSTEM_PATH_DIRECTORY) {
        WindowsWalkContext child = *context;

        if (context->depth >= SYSTEM_MAX_TREE_DEPTH) {
            return CUP_ERR_FILESYSTEM;
        }
        child.depth++;
        err = list_directory_bound(path, identity, windows_walk_entry, &child);
        if (err != CUP_OK) {
            return err;
        }
    }
    return context->callback(path, kind, identity, context->userdata);
}

static CupError walk_directory_bound(const char *path,
                                     const SystemPathIdentity *expected_identity,
                                     SystemDirectoryCallback callback,
                                     void *userdata) {
    WindowsWalkContext context;
    if (callback == NULL || text_is_empty(path)) {
        return CUP_ERR_INVALID_INPUT;
    }
    context.callback = callback;
    context.userdata = userdata;
    context.depth = 0;
    return list_directory_bound(path, expected_identity, windows_walk_entry, &context);
}

CupError system_walk_directory(const char *path,
                               SystemDirectoryCallback callback,
                               void *userdata) {
    return walk_directory_bound(path, NULL, callback, userdata);
}

/* Recursive mutations reject reparse points through the directory walker. */
typedef struct {
    int (*cancelled)(void);
} RemoveTreeContext;

static CupError remove_tree_callback(const char *path,
                                     SystemPathKind kind,
                                     const SystemPathIdentity *identity,
                                     void *userdata) {
    RemoveTreeContext *context = userdata;

    if (identity == NULL || !identity->valid || identity->kind != kind) {
        return CUP_ERR_FILESYSTEM;
    }
    if (context != NULL && context->cancelled != NULL && context->cancelled()) {
        return CUP_ERR_INTERRUPT;
    }
    return remove_path_handle_bound(
        path, kind == SYSTEM_PATH_DIRECTORY, identity);
}

typedef struct {
    const char *preserve_name;
    int (*cancelled)(void);
} RemoveTreeContentsContext;

static CupError remove_tree_contents_callback(const char *path,
                                              SystemPathKind kind,
                                              const SystemPathIdentity *identity,
                                              void *userdata) {
    RemoveTreeContentsContext *context = userdata;
    const char *name = path_last_segment(path);

    if (context == NULL || name == NULL || identity == NULL || !identity->valid ||
        identity->kind != kind) {
        return CUP_ERR_INVALID_INPUT;
    }
    if (context->preserve_name != NULL && strcmp(name, context->preserve_name) == 0) {
        return CUP_OK;
    }
    return system_remove_path_if_identity(path, identity, context->cancelled);
}

CupError system_remove_tree_contents(const char *path,
                                     const char *preserve_name,
                                     int (*cancelled)(void)) {
    RemoveTreeContentsContext context;
    SystemPathKind kind;
    CupError err;

    if (text_is_empty(path) ||
        (preserve_name != NULL && !path_is_safe_segment(preserve_name))) {
        return CUP_ERR_INVALID_INPUT;
    }
    if (cancelled != NULL && cancelled()) {
        return CUP_ERR_INTERRUPT;
    }

    err = system_get_path_kind(path, &kind);
    if (err != CUP_OK || kind != SYSTEM_PATH_DIRECTORY) {
        return err == CUP_OK ? CUP_ERR_FILESYSTEM : err;
    }

    context.preserve_name = preserve_name;
    context.cancelled = cancelled;
    return system_list_directory(path, remove_tree_contents_callback, &context);
}

static CupError remove_tree_common(const char *path,
                                   const SystemPathIdentity *expected_identity,
                                   int (*cancelled)(void)) {
    SystemPathIdentity root_identity;
    RemoveTreeContext context;
    CupError err;

    if (text_is_empty(path) ||
        (expected_identity != NULL && !expected_identity->valid)) {
        return CUP_ERR_INVALID_INPUT;
    }
    if (cancelled != NULL && cancelled()) {
        return CUP_ERR_INTERRUPT;
    }

    err = system_get_path_identity(path, &root_identity);
    if (err != CUP_OK || !root_identity.valid) {
        return err != CUP_OK || expected_identity == NULL ? err : CUP_ERR_FILESYSTEM;
    }
    if (expected_identity != NULL &&
        !system_path_identity_equal(&root_identity, expected_identity)) {
        return CUP_ERR_TRANSACTION;
    }
    if (root_identity.kind != SYSTEM_PATH_DIRECTORY) {
        return remove_path_handle_bound(path, 0, &root_identity);
    }

    context.cancelled = cancelled;
    err = walk_directory_bound(path, &root_identity, remove_tree_callback, &context);
    if (err != CUP_OK) {
        return err;
    }
    return remove_path_handle_bound(path, 1, &root_identity);
}

CupError system_remove_path_if_identity(const char *path,
                                        const SystemPathIdentity *expected_identity,
                                        int (*cancelled)(void)) {
    if (expected_identity == NULL || !expected_identity->valid) {
        return CUP_ERR_INVALID_INPUT;
    }
    if (expected_identity->kind == SYSTEM_PATH_DIRECTORY) {
        return remove_tree_common(path, expected_identity, cancelled);
    }
    if (cancelled != NULL && cancelled()) {
        return CUP_ERR_INTERRUPT;
    }
    return remove_path_handle_bound(path, 0, expected_identity);
}

CupError system_remove_tree(const char *path, int (*cancelled)(void)) {
    return remove_tree_common(path, NULL, cancelled);
}

/* Nonblocking file locks backed by a process-owned Windows handle. */
static CupError lock_acquire_common(SystemLock *lock,
                                    const char *path,
                                    SystemLockMode mode,
                                    int create) {
    wchar_t wide_path[MAX_PATH_LEN];
    HANDLE handle;
    OVERLAPPED overlapped;
    BY_HANDLE_FILE_INFORMATION info;
    DWORD flags = LOCKFILE_FAIL_IMMEDIATELY;
    DWORD disposition;
    CupError err;

    if (lock == NULL || lock->active || text_is_empty(path) ||
        (mode != SYSTEM_LOCK_SHARED && mode != SYSTEM_LOCK_EXCLUSIVE) ||
        (create != 0 && create != 1)) {
        return CUP_ERR_INVALID_INPUT;
    }
    memset(lock, 0, sizeof(*lock));
    err = validate_parent_directory_chain(path);
    if (err != CUP_OK) {
        return err;
    }
    err = windows_utf8_to_wide_path(path, wide_path, MAX_PATH_LEN);
    if (err != CUP_OK) {
        return err;
    }

    disposition = create ? OPEN_ALWAYS : OPEN_EXISTING;
    handle = CreateFileW(wide_path,
                         mode == SYSTEM_LOCK_SHARED ? GENERIC_READ : GENERIC_READ | GENERIC_WRITE,
                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         NULL,
                         disposition,
                         FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
                         NULL);
    if (handle == INVALID_HANDLE_VALUE) {
        DWORD error = GetLastError();

        return error == ERROR_SHARING_VIOLATION || error == ERROR_LOCK_VIOLATION
                   ? CUP_ERR_LOCK
                   : CUP_ERR_FILESYSTEM;
    }
    if (!GetFileInformationByHandle(handle, &info) ||
        (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0) {
        CloseHandle(handle);
        return CUP_ERR_FILESYSTEM;
    }

    if (mode == SYSTEM_LOCK_EXCLUSIVE) {
        flags |= LOCKFILE_EXCLUSIVE_LOCK;
    }
    /* SystemLock is an advisory coordination primitive. Lock one sentinel byte
     * beyond ordinary CUP lock-file contents so Windows byte-range locking does
     * not make marker/config contents unreadable through independent handles. */
    ZeroMemory(&overlapped, sizeof(overlapped));
    overlapped.OffsetHigh = 1;
    if (!LockFileEx(handle, flags, 0, 1, 0, &overlapped)) {
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

CupError system_lock_acquire(SystemLock *lock, const char *path, SystemLockMode mode) {
    return lock_acquire_common(lock, path, mode, mode == SYSTEM_LOCK_EXCLUSIVE);
}

CupError system_lock_acquire_existing(SystemLock *lock,
                                      const char *path,
                                      SystemLockMode mode) {
    return lock_acquire_common(lock, path, mode, 0);
}

CupError system_lock_get_identity(const SystemLock *lock, SystemPathIdentity *identity) {
    BY_HANDLE_FILE_INFORMATION info;

    if (lock == NULL || !lock->active || identity == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    memset(identity, 0, sizeof(*identity));
    if (!GetFileInformationByHandle((HANDLE)lock->handle, &info)) {
        return CUP_ERR_FILESYSTEM;
    }

    return identity_from_handle_information((HANDLE)lock->handle, &info, identity);
}

CupError system_lock_read(const SystemLock *lock,
                          void *buffer,
                          size_t capacity,
                          size_t *size) {
    LARGE_INTEGER beginning;
    size_t total = 0;

    if (size == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    *size = 0;
    if (lock == NULL || !lock->active || buffer == NULL || capacity == 0 ||
        capacity > MAXDWORD) {
        return CUP_ERR_INVALID_INPUT;
    }

    beginning.QuadPart = 0;
    if (!SetFilePointerEx((HANDLE)lock->handle, beginning, NULL, FILE_BEGIN)) {
        return CUP_ERR_FILESYSTEM;
    }
    while (total < capacity) {
        DWORD read_size = 0;

        if (!ReadFile((HANDLE)lock->handle,
                      (unsigned char *)buffer + total,
                      (DWORD)(capacity - total),
                      &read_size,
                      NULL)) {
            return CUP_ERR_FILESYSTEM;
        }
        if (read_size == 0) {
            break;
        }
        total += (size_t)read_size;
    }

    *size = total;
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
    overlapped.OffsetHigh = 1;
    UnlockFileEx(handle, 0, 1, 0, &overlapped);
    CloseHandle(handle);
    lock->handle = 0;
    lock->active = 0;
}
