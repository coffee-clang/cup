#include "system.h"

#include "constants.h"
#include "path.h"
#include "text.h"

#include <ctype.h>
#include <fcntl.h>
#include <io.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>
#include <windows.h>

// INTERNAL HELPERS
static CupError utf8_to_wide(const char *input, wchar_t *output, size_t output_count) {
    int written;

    if (text_is_empty(input) || output == NULL || output_count == 0 || output_count > INT_MAX) {
        return CUP_ERR_INVALID_INPUT;
    }

    written = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input, -1,
        output, (int)output_count);
    return written == 0 ? CUP_ERR_FILESYSTEM : CUP_OK;
}

static CupError wide_to_utf8(const wchar_t *input, char *output, size_t output_size) {
    int written;

    if (input == NULL || output == NULL || output_size == 0 || output_size > INT_MAX) {
        return CUP_ERR_INVALID_INPUT;
    }

    written = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, input, -1,
        output, (int)output_size, NULL, NULL);
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
        NULL, error_code, 0, wide_message,
        (DWORD)(sizeof(wide_message) / sizeof(wide_message[0])), NULL);
    while (length > 0 && (wide_message[length - 1] == L'\r' ||
        wide_message[length - 1] == L'\n' || wide_message[length - 1] == L' ' ||
        wide_message[length - 1] == L'\t')) {
        wide_message[--length] = L'\0';
    }

    if (length == 0 || wide_to_utf8(wide_message, error_message,
        sizeof(error_message)) != CUP_OK) {
        text_format(error_message, sizeof(error_message),
            "Windows error code %lu", (unsigned long)error_code);
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

        while (*left != '\0' && *right != '\0' &&
            tolower(*left) == tolower(*right)) {
            left++;
            right++;
        }
        if (*left == '\0' && *right == '\0') {
            return 1;
        }
    }

    return 0;
}

static CupError build_temp_candidate(const char *directory, const char *prefix,
    unsigned long attempt, char *path, size_t path_size) {
    ULONGLONG tick = GetTickCount64();
    DWORD pid = GetCurrentProcessId();

    return text_format(path, path_size, "%s/%s-%lu-%llu-%lu.tmp",
        directory, prefix, (unsigned long)pid, (unsigned long long)tick, attempt);
}

static CupError open_temp_handle(const char *directory, const char *prefix,
    char *path, size_t path_size, HANDLE *handle) {
    unsigned long attempt;

    if (text_is_empty(directory) || text_is_empty(prefix) || path == NULL ||
        path_size == 0 || handle == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    for (attempt = 0; attempt < 256; ++attempt) {
        wchar_t wide_path[MAX_PATH_LEN];

        if (build_temp_candidate(directory, prefix, attempt, path, path_size) != CUP_OK ||
            utf8_to_wide(path, wide_path, MAX_PATH_LEN) != CUP_OK) {
            return CUP_ERR_TEMPORARY;
        }

        *handle = CreateFileW(wide_path, GENERIC_READ | GENERIC_WRITE, 0, NULL,
            CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY, NULL);
        if (*handle != INVALID_HANDLE_VALUE) {
            return CUP_OK;
        }
        if (GetLastError() != ERROR_FILE_EXISTS && GetLastError() != ERROR_ALREADY_EXISTS) {
            return CUP_ERR_TEMPORARY;
        }
    }

    return CUP_ERR_TEMPORARY;
}

// PROCESS AND ENVIRONMENT
CupError system_get_home_dir(char *buffer, size_t size) {
    wchar_t value[MAX_PATH_LEN];
    DWORD length;
    int absolute;

    if (buffer == NULL || size == 0) {
        return CUP_ERR_INVALID_INPUT;
    }

    length = GetEnvironmentVariableW(L"USERPROFILE", value, MAX_PATH_LEN);
    if (length == 0 || length >= MAX_PATH_LEN) {
        print_windows_error("could not read USERPROFILE", NULL);
        return CUP_ERR_FILESYSTEM;
    }

    absolute = (iswalpha(value[0]) && value[1] == L':' &&
        (value[2] == L'\\' || value[2] == L'/')) ||
        (value[0] == L'\\' && value[1] == L'\\');
    if (!absolute) {
        fprintf(stderr, "Error: USERPROFILE must contain an absolute path.\n");
        return CUP_ERR_FILESYSTEM;
    }

    return wide_to_utf8(value, buffer, size);
}

CupError system_start_uninstall(const char *cup_root, const char *uninstall_script) {
    wchar_t temp_directory_wide[MAX_PATH_LEN];
    wchar_t temp_script_wide[MAX_PATH_LEN];
    wchar_t wide_root[MAX_PATH_LEN];
    wchar_t wide_command[MAX_PATH_LEN * 4];
    char temp_directory[MAX_PATH_LEN];
    char temp_script[MAX_PATH_LEN];
    FILE *file = NULL;
    STARTUPINFOW startup;
    PROCESS_INFORMATION process;
    DWORD length;
    int written;

    if (text_is_empty(cup_root) || text_is_empty(uninstall_script)) {
        return CUP_ERR_INVALID_INPUT;
    }

    length = GetTempPathW(MAX_PATH_LEN, temp_directory_wide);
    if (length == 0 || length >= MAX_PATH_LEN ||
        wide_to_utf8(temp_directory_wide, temp_directory, sizeof(temp_directory)) != CUP_OK ||
        system_create_temp_file(temp_directory, "cup-uninstall", temp_script,
            sizeof(temp_script), &file) != CUP_OK) {
        print_windows_error("could not create temporary uninstall path", NULL);
        return CUP_ERR_FILESYSTEM;
    }

    if (fclose(file) != 0 || system_copy_file(uninstall_script, temp_script) != CUP_OK ||
        utf8_to_wide(temp_script, temp_script_wide, MAX_PATH_LEN) != CUP_OK ||
        utf8_to_wide(cup_root, wide_root, MAX_PATH_LEN) != CUP_OK) {
        system_remove_file(temp_script);
        return CUP_ERR_FILESYSTEM;
    }

    written = _snwprintf(wide_command,
        sizeof(wide_command) / sizeof(wide_command[0]),
        L"powershell.exe -NoProfile -ExecutionPolicy Bypass "
        L"-File \"%ls\" -CupRoot \"%ls\" -SelfPath \"%ls\"",
        temp_script_wide, wide_root, temp_script_wide);
    if (written < 0 || (size_t)written >=
        sizeof(wide_command) / sizeof(wide_command[0])) {
        system_remove_file(temp_script);
        return CUP_ERR_BUFFER_TOO_SMALL;
    }

    ZeroMemory(&startup, sizeof(startup));
    startup.cb = sizeof(startup);
    ZeroMemory(&process, sizeof(process));

    if (!CreateProcessW(NULL, wide_command, NULL, NULL, FALSE, 0, NULL, NULL,
        &startup, &process)) {
        print_windows_error("could not start uninstall process", temp_script);
        system_remove_file(temp_script);
        return CUP_ERR_FILESYSTEM;
    }

    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return CUP_OK;
}

// FILES AND DIRECTORIES
CupError system_make_directory(const char *path) {
    wchar_t wide_path[MAX_PATH_LEN];
    SystemPathKind info;

    if (utf8_to_wide(path, wide_path, MAX_PATH_LEN) != CUP_OK) {
        return CUP_ERR_INVALID_INPUT;
    }
    if (CreateDirectoryW(wide_path, NULL)) {
        return CUP_OK;
    }
    if (GetLastError() != ERROR_ALREADY_EXISTS ||
        system_get_path_kind(path, &info) != CUP_OK ||
        info != SYSTEM_PATH_DIRECTORY) {
        print_windows_error("could not create directory", path);
        return CUP_ERR_FILESYSTEM;
    }
    return CUP_OK;
}

CupError system_remove_directory(const char *path) {
    wchar_t wide_path[MAX_PATH_LEN];
    SystemPathKind info;

    if (utf8_to_wide(path, wide_path, MAX_PATH_LEN) != CUP_OK) {
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

static CupError move_path_with_flags(const char *source, const char *destination,
    DWORD flags, SystemCommitState *commit_state) {
    wchar_t wide_source[MAX_PATH_LEN];
    wchar_t wide_destination[MAX_PATH_LEN];

    if (commit_state == NULL || utf8_to_wide(source, wide_source, MAX_PATH_LEN) != CUP_OK ||
        utf8_to_wide(destination, wide_destination, MAX_PATH_LEN) != CUP_OK) {
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

CupError system_move_path(const char *source, const char *destination,
    SystemCommitState *commit_state) {
    return move_path_with_flags(source, destination, 0, commit_state);
}

CupError system_replace_file(const char *source, const char *destination,
    SystemCommitState *commit_state) {
    return move_path_with_flags(source, destination, MOVEFILE_REPLACE_EXISTING,
        commit_state);
}

CupError system_remove_file(const char *path) {
    wchar_t wide_path[MAX_PATH_LEN];
    DWORD attributes;

    if (utf8_to_wide(path, wide_path, MAX_PATH_LEN) != CUP_OK) {
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
    wchar_t source[MAX_PATH_LEN];
    wchar_t destination[MAX_PATH_LEN];
    SystemPathKind source_info;
    SystemPathKind destination_info;

    if (utf8_to_wide(source_path, source, MAX_PATH_LEN) != CUP_OK ||
        utf8_to_wide(destination_path, destination, MAX_PATH_LEN) != CUP_OK ||
        system_get_path_kind(source_path, &source_info) != CUP_OK ||
        system_get_path_kind(destination_path, &destination_info) != CUP_OK) {
        return CUP_ERR_INVALID_INPUT;
    }
    if (source_info != SYSTEM_PATH_REGULAR_FILE ||
        destination_info == SYSTEM_PATH_LINK ||
        destination_info == SYSTEM_PATH_DIRECTORY) {
        return CUP_ERR_FILESYSTEM;
    }
    if (!CopyFileW(source, destination, FALSE)) {
        print_windows_error("could not copy file", source_path);
        return CUP_ERR_FILESYSTEM;
    }
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
    if (text_is_empty(path)) {
        return CUP_ERR_INVALID_INPUT;
    }
    return CUP_OK;
}

// TEMPORARY OBJECTS
CupError system_create_temp_file(const char *directory, const char *prefix,
    char *path, size_t path_size, FILE **file) {
    HANDLE handle;
    int descriptor;

    if (file == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    *file = NULL;

    if (open_temp_handle(directory, prefix, path, path_size, &handle) != CUP_OK) {
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

CupError system_create_temp_directory(const char *directory, const char *prefix,
    char *path, size_t path_size) {
    unsigned long attempt;

    if (text_is_empty(directory) || text_is_empty(prefix) || path == NULL ||
        path_size == 0) {
        return CUP_ERR_INVALID_INPUT;
    }

    for (attempt = 0; attempt < 256; ++attempt) {
        wchar_t wide_path[MAX_PATH_LEN];

        if (build_temp_candidate(directory, prefix, attempt, path, path_size) != CUP_OK ||
            utf8_to_wide(path, wide_path, MAX_PATH_LEN) != CUP_OK) {
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

CupError system_make_unique_temp_path(const char *directory, const char *prefix,
    char *path, size_t path_size) {
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

// PATH INSPECTION
CupError system_get_path_kind(const char *path, SystemPathKind *path_kind) {
    wchar_t wide_path[MAX_PATH_LEN];
    DWORD attributes;

    if (path_kind == NULL ||
        utf8_to_wide(path, wide_path, MAX_PATH_LEN) != CUP_OK) {
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

CupError system_path_exists(const char *path, int *exists) {
    SystemPathKind info;
    CupError err;

    if (exists == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    err = system_get_path_kind(path, &info);
    if (err != CUP_OK) {
        return err;
    }
    *exists = info != SYSTEM_PATH_MISSING;
    return CUP_OK;
}

CupError system_is_directory(const char *path, int *is_directory) {
    SystemPathKind info;
    CupError err;

    if (is_directory == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    err = system_get_path_kind(path, &info);
    if (err != CUP_OK) {
        return err;
    }
    *is_directory = info == SYSTEM_PATH_DIRECTORY;
    return CUP_OK;
}

CupError system_is_regular_file(const char *path, int *is_regular_file) {
    SystemPathKind info;
    CupError err;

    if (is_regular_file == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    err = system_get_path_kind(path, &info);
    if (err != CUP_OK) {
        return err;
    }
    *is_regular_file = info == SYSTEM_PATH_REGULAR_FILE;
    return CUP_OK;
}

CupError system_file_size(const char *path, long long *file_size) {
    wchar_t wide_path[MAX_PATH_LEN];
    HANDLE file;
    LARGE_INTEGER value;
    SystemPathKind info;

    if (file_size == NULL || utf8_to_wide(path, wide_path, MAX_PATH_LEN) != CUP_OK ||
        system_get_path_kind(path, &info) != CUP_OK ||
        info != SYSTEM_PATH_REGULAR_FILE) {
        return CUP_ERR_INVALID_INPUT;
    }
    file = CreateFileW(wide_path, GENERIC_READ, FILE_SHARE_READ, NULL,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
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

// PERMISSIONS
CupError system_is_executable(const char *path, int *is_executable) {
    SystemPathKind info;
    CupError err;

    if (is_executable == NULL || text_is_empty(path)) {
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

    if (is_read_only == NULL || utf8_to_wide(path, wide_path, MAX_PATH_LEN) != CUP_OK ||
        system_get_path_kind(path, &info) != CUP_OK ||
        (info != SYSTEM_PATH_REGULAR_FILE && info != SYSTEM_PATH_DIRECTORY)) {
        return CUP_ERR_INVALID_INPUT;
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

    if (utf8_to_wide(path, wide_path, MAX_PATH_LEN) != CUP_OK ||
        system_get_path_kind(path, &info) != CUP_OK ||
        (info != SYSTEM_PATH_REGULAR_FILE && info != SYSTEM_PATH_DIRECTORY)) {
        return CUP_ERR_INVALID_INPUT;
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

// DIRECTORY TRAVERSAL
CupError system_list_directory(const char *path, SystemDirectoryCallback callback, void *userdata) {
    wchar_t wide_path[MAX_PATH_LEN];
    wchar_t pattern[MAX_PATH_LEN];
    WIN32_FIND_DATAW data;
    HANDLE handle;
    CupError err;
    SystemPathKind root_info;

    if (callback == NULL || utf8_to_wide(path, wide_path, MAX_PATH_LEN) != CUP_OK) {
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

typedef struct {
    SystemDirectoryCallback callback;
    void *userdata;
} WalkContext;

static CupError walk_directory_entry(const char *path, SystemPathKind path_kind, void *userdata) {
    WalkContext *context = userdata;
    CupError err;

    if (context == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    if (path_kind == SYSTEM_PATH_DIRECTORY) {
        err = system_walk_directory(path, context->callback, context->userdata);
        if (err != CUP_OK) {
            return err;
        }
    }
    return context->callback(path, path_kind, context->userdata);
}

CupError system_walk_directory(const char *path, SystemDirectoryCallback callback, void *userdata) {
    WalkContext context;

    if (callback == NULL || text_is_empty(path)) {
        return CUP_ERR_INVALID_INPUT;
    }
    context.callback = callback;
    context.userdata = userdata;
    return system_list_directory(path, walk_directory_entry, &context);
}

// FILE LOCKING
CupError system_lock_acquire(SystemLock *lock, const char *path, SystemLockMode mode) {
    wchar_t wide_path[MAX_PATH_LEN];
    HANDLE handle;
    OVERLAPPED overlapped;
    BY_HANDLE_FILE_INFORMATION info;
    DWORD flags = LOCKFILE_FAIL_IMMEDIATELY;

    if (lock == NULL || utf8_to_wide(path, wide_path, MAX_PATH_LEN) != CUP_OK) {
        return CUP_ERR_INVALID_INPUT;
    }
    memset(lock, 0, sizeof(*lock));

    handle = CreateFileW(wide_path, GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
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
