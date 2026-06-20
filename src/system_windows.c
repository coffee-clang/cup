#include "system.h"

#include "constants.h"
#include "path.h"
#include "util.h"

#include <fcntl.h>
#include <io.h>
#include <limits.h>
#include <wchar.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>

// INTERNAL HELPERS
static CupError utf8_to_wide(const char *input, wchar_t *output, size_t output_count) {
    int written;

    if (is_empty_string(input) || output == NULL || output_count == 0 || output_count > INT_MAX) {
        return CUP_ERR_INVALID_INPUT;
    }

    written = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input, -1, output, (int)output_count);
    if (written == 0) {
        return CUP_ERR_FILESYSTEM;
    }
    return CUP_OK;
}

static CupError wide_to_utf8(const wchar_t *input, char *output, size_t output_size) {
    int written;

    if (input == NULL || output == NULL || output_size == 0 || output_size > INT_MAX) {
        return CUP_ERR_INVALID_INPUT;
    }

    written = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, input, -1, output, (int)output_size, NULL, NULL);
    if (written == 0) {
        return CUP_ERR_FILESYSTEM;
    }
    return CUP_OK;
}

static void print_windows_error(const char *message, const char *path) {
    DWORD error_code = GetLastError();
    wchar_t wide_message[512];
    char error_message[1024];
    DWORD length;

    if (is_empty_string(message)) {
        message = "Windows operation";
    }

    wide_message[0] = L'\0';
    length = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, error_code, 0,
        wide_message, (DWORD)(sizeof(wide_message) / sizeof(wide_message[0])), NULL);
    while (length > 0 && (wide_message[length - 1] == L'\r' || wide_message[length - 1] == L'\n' ||
        wide_message[length - 1] == L' ' || wide_message[length - 1] == L'\t')) {
        wide_message[--length] = L'\0';
    }

    if (length == 0 || wide_to_utf8(wide_message, error_message, sizeof(error_message)) != CUP_OK) {
        checked_snprintf(error_message, sizeof(error_message), "Windows error code %lu", (unsigned long)error_code);
    }

    if (is_empty_string(path)) {
        fprintf(stderr, "Error: %s failed: %s.\n", message, error_message);
    } else {
        fprintf(stderr, "Error: %s '%s' failed: %s.\n", message, path, error_message);
    }
}

static CupError get_attributes(const char *path, DWORD *attributes) {
    wchar_t wide_path[MAX_PATH_LEN];

    if (attributes == NULL || utf8_to_wide(path, wide_path, MAX_PATH_LEN) != CUP_OK) {
        return CUP_ERR_INVALID_INPUT;
    }
    *attributes = GetFileAttributesW(wide_path);
    return CUP_OK;
}

static void set_path_info(DWORD attributes, SystemPathInfo *info) {
    info->is_directory = (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    info->is_reparse_point = (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
    info->is_regular_file = !info->is_directory && !info->is_reparse_point;
}

// PROCESS AND ENVIRONMENT
CupError system_get_home_dir(char *buffer, size_t size) {
    wchar_t value[MAX_PATH_LEN];
    DWORD length;

    if (buffer == NULL || size == 0) {
        return CUP_ERR_INVALID_INPUT;
    }

    length = GetEnvironmentVariableW(L"USERPROFILE", value, MAX_PATH_LEN);
    if (length == 0 || length >= MAX_PATH_LEN) {
        print_windows_error("could not read USERPROFILE", NULL);
        return CUP_ERR_FILESYSTEM;
    }
    return wide_to_utf8(value, buffer, size);
}

CupError system_get_process_id(char *buffer, size_t size) {
    if (buffer == NULL || size == 0) {
        return CUP_ERR_INVALID_INPUT;
    }
    return checked_snprintf(buffer, size, "%lu", (unsigned long)GetCurrentProcessId());
}

CupError system_start_uninstall(const char *cup_root, const char *uninstall_script) {
    wchar_t temp_directory[MAX_PATH_LEN];
    wchar_t temp_file[MAX_PATH_LEN];
    wchar_t temp_script[MAX_PATH_LEN];
    wchar_t wide_root[MAX_PATH_LEN];
    wchar_t wide_command[MAX_PATH_LEN * 4];
    char temp_file_utf8[MAX_PATH_LEN];
    STARTUPINFOW startup;
    PROCESS_INFORMATION process;
    DWORD length;
    int written;

    if (is_empty_string(cup_root) || is_empty_string(uninstall_script)) {
        return CUP_ERR_INVALID_INPUT;
    }

    length = GetTempPathW(MAX_PATH_LEN, temp_directory);
    if (length == 0 || length >= MAX_PATH_LEN || GetTempFileNameW(temp_directory, L"cup", 0, temp_file) == 0) {
        print_windows_error("could not create temporary uninstall path", NULL);
        return CUP_ERR_FILESYSTEM;
    }

    written = _snwprintf(temp_script, MAX_PATH_LEN, L"%ls.ps1", temp_file);
    if (written < 0 || written >= MAX_PATH_LEN) {
        DeleteFileW(temp_file);
        return CUP_ERR_BUFFER_TOO_SMALL;
    }
    if (!MoveFileExW(temp_file, temp_script, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temp_file);
        return CUP_ERR_FILESYSTEM;
    }
    if (wide_to_utf8(temp_script, temp_file_utf8, sizeof(temp_file_utf8)) != CUP_OK ||
        system_copy_file(uninstall_script, temp_file_utf8) != CUP_OK ||
        utf8_to_wide(cup_root, wide_root, MAX_PATH_LEN) != CUP_OK) {
        DeleteFileW(temp_script);
        return CUP_ERR_FILESYSTEM;
    }

    written = _snwprintf(wide_command, sizeof(wide_command) / sizeof(wide_command[0]),
        L"powershell.exe -NoProfile -ExecutionPolicy Bypass -File \"%ls\" -CupRoot \"%ls\" -SelfPath \"%ls\"",
        temp_script, wide_root, temp_script);
    if (written < 0 || (size_t)written >= sizeof(wide_command) / sizeof(wide_command[0])) {
        DeleteFileW(temp_script);
        return CUP_ERR_BUFFER_TOO_SMALL;
    }

    ZeroMemory(&startup, sizeof(startup));
    startup.cb = sizeof(startup);
    ZeroMemory(&process, sizeof(process));

    if (!CreateProcessW(NULL, wide_command, NULL, NULL, FALSE, 0, NULL, NULL, &startup, &process)) {
        print_windows_error("could not start uninstall process", temp_file_utf8);
        DeleteFileW(temp_script);
        return CUP_ERR_FILESYSTEM;
    }

    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return CUP_OK;
}

// FILES AND DIRECTORIES
CupError system_make_directory(const char *path) {
    wchar_t wide_path[MAX_PATH_LEN];

    if (utf8_to_wide(path, wide_path, MAX_PATH_LEN) != CUP_OK) {
        return CUP_ERR_INVALID_INPUT;
    }
    if (!CreateDirectoryW(wide_path, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) {
        print_windows_error("could not create directory", path);
        return CUP_ERR_FILESYSTEM;
    }
    return CUP_OK;
}

CupError system_remove_directory(const char *path) {
    wchar_t wide_path[MAX_PATH_LEN];

    if (utf8_to_wide(path, wide_path, MAX_PATH_LEN) != CUP_OK) {
        return CUP_ERR_INVALID_INPUT;
    }
    if (!RemoveDirectoryW(wide_path)) {
        DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
            return CUP_OK;
        }
        print_windows_error("could not remove directory", path);
        return CUP_ERR_FILESYSTEM;
    }
    return CUP_OK;
}

static CupError move_path_with_flags(const char *source, const char *destination, DWORD flags) {
    wchar_t wide_source[MAX_PATH_LEN];
    wchar_t wide_destination[MAX_PATH_LEN];

    if (utf8_to_wide(source, wide_source, MAX_PATH_LEN) != CUP_OK ||
        utf8_to_wide(destination, wide_destination, MAX_PATH_LEN) != CUP_OK) {
        return CUP_ERR_INVALID_INPUT;
    }
    if (!MoveFileExW(wide_source, wide_destination, flags | MOVEFILE_WRITE_THROUGH)) {
        print_windows_error("could not move path", source);
        return CUP_ERR_FILESYSTEM;
    }
    return CUP_OK;
}

CupError system_move_path(const char *source, const char *destination) {
    return move_path_with_flags(source, destination, MOVEFILE_WRITE_THROUGH);
}

CupError system_replace_file(const char *source, const char *destination) {
    return move_path_with_flags(source, destination, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
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
    if (attributes & FILE_ATTRIBUTE_DIRECTORY) {
        return CUP_ERR_FILESYSTEM;
    }
    if (attributes & FILE_ATTRIBUTE_READONLY) {
        if (!SetFileAttributesW(wide_path, attributes & ~FILE_ATTRIBUTE_READONLY)) {
            return CUP_ERR_FILESYSTEM;
        }
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

    if (utf8_to_wide(source_path, source, MAX_PATH_LEN) != CUP_OK ||
        utf8_to_wide(destination_path, destination, MAX_PATH_LEN) != CUP_OK) {
        return CUP_ERR_INVALID_INPUT;
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

// PATH INSPECTION
CupError system_path_exists(const char *path, int *exists) {
    DWORD attributes;

    if (exists == NULL || get_attributes(path, &attributes) != CUP_OK) {
        return CUP_ERR_INVALID_INPUT;
    }
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
            *exists = 0;
            return CUP_OK;
        }
        return CUP_ERR_FILESYSTEM;
    }
    *exists = 1;
    return CUP_OK;
}

CupError system_is_directory(const char *path, int *is_directory) {
    DWORD attributes;
    int exists;

    if (is_directory == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    if (system_path_exists(path, &exists) != CUP_OK) {
        return CUP_ERR_FILESYSTEM;
    }
    if (!exists) {
        *is_directory = 0;
        return CUP_OK;
    }
    get_attributes(path, &attributes);
    *is_directory = (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    return CUP_OK;
}

CupError system_is_regular_file(const char *path, int *is_regular_file) {
    DWORD attributes;
    int exists;

    if (is_regular_file == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    if (system_path_exists(path, &exists) != CUP_OK) {
        return CUP_ERR_FILESYSTEM;
    }
    if (!exists) {
        *is_regular_file = 0;
        return CUP_OK;
    }
    get_attributes(path, &attributes);
    *is_regular_file = (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) == 0;
    return CUP_OK;
}

CupError system_file_size(const char *path, long long *file_size) {
    wchar_t wide_path[MAX_PATH_LEN];
    HANDLE file;
    LARGE_INTEGER value;

    if (file_size == NULL || utf8_to_wide(path, wide_path, MAX_PATH_LEN) != CUP_OK) {
        return CUP_ERR_INVALID_INPUT;
    }
    file = CreateFileW(wide_path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
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
    int regular;
    CupError err;

    if (is_executable == NULL || is_empty_string(path)) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = system_is_regular_file(path, &regular);
    if (err != CUP_OK) {
        return err;
    }
    *is_executable = regular;
    return CUP_OK;
}

CupError system_is_read_only(const char *path, int *is_read_only) {
    DWORD attributes;

    if (is_read_only == NULL || get_attributes(path, &attributes) != CUP_OK || attributes == INVALID_FILE_ATTRIBUTES) {
        return CUP_ERR_INVALID_INPUT;
    }
    *is_read_only = (attributes & FILE_ATTRIBUTE_READONLY) != 0;
    return CUP_OK;
}

CupError system_set_read_only(const char *path, int read_only) {
    wchar_t wide_path[MAX_PATH_LEN];
    DWORD attributes;

    if (utf8_to_wide(path, wide_path, MAX_PATH_LEN) != CUP_OK) {
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
    if (!SetFileAttributesW(wide_path, attributes)) {
        return CUP_ERR_FILESYSTEM;
    }
    return CUP_OK;
}

CupError system_set_executable(const char *path, int executable) {
    (void)executable;
    if (is_empty_string(path)) {
        return CUP_ERR_INVALID_INPUT;
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

    if (callback == NULL || utf8_to_wide(path, wide_path, MAX_PATH_LEN) != CUP_OK) {
        return CUP_ERR_INVALID_INPUT;
    }
    if (_snwprintf(pattern, MAX_PATH_LEN, L"%ls\\*", wide_path) < 0) {
        return CUP_ERR_BUFFER_TOO_SMALL;
    }

    handle = FindFirstFileW(pattern, &data);
    if (handle == INVALID_HANDLE_VALUE) {
        DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
            return CUP_OK;
        }
        return CUP_ERR_FILESYSTEM;
    }

    do {
        char name[MAX_PATH_LEN];
        char child[MAX_PATH_LEN];
        SystemPathInfo info;

        if (wcscmp(data.cFileName, L".") == 0 || wcscmp(data.cFileName, L"..") == 0) {
            continue;
        }
        if (wide_to_utf8(data.cFileName, name, sizeof(name)) != CUP_OK ||
            path_join(child, sizeof(child), path, name) != CUP_OK) {
            FindClose(handle);
            return CUP_ERR_FILESYSTEM;
        }
        set_path_info(data.dwFileAttributes, &info);
        err = callback(child, &info, userdata);
        if (err != CUP_OK) {
            FindClose(handle);
            return err;
        }
    } while (FindNextFileW(handle, &data));

    if (GetLastError() != ERROR_NO_MORE_FILES) {
        FindClose(handle);
        return CUP_ERR_FILESYSTEM;
    }
    FindClose(handle);
    return CUP_OK;
}

typedef struct {
    SystemDirectoryCallback callback;
    void *userdata;
} WalkContext;

static CupError walk_directory_entry(const char *path, const SystemPathInfo *info, void *userdata) {
    WalkContext *context = userdata;
    CupError err;

    if (context == NULL || info == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    if (info->is_directory && !info->is_reparse_point) {
        err = system_walk_directory(path, context->callback, context->userdata);
        if (err != CUP_OK) {
            return err;
        }
    }
    return context->callback(path, info, context->userdata);
}

CupError system_walk_directory(const char *path, SystemDirectoryCallback callback, void *userdata) {
    WalkContext context;

    if (callback == NULL || is_empty_string(path)) {
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
    DWORD flags = LOCKFILE_FAIL_IMMEDIATELY;

    if (lock == NULL || utf8_to_wide(path, wide_path, MAX_PATH_LEN) != CUP_OK) {
        return CUP_ERR_INVALID_INPUT;
    }
    memset(lock, 0, sizeof(*lock));

    handle = CreateFileW(wide_path, GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (handle == INVALID_HANDLE_VALUE) {
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
