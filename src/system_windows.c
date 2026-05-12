#include "system.h"
#include "constants.h"
#include "util.h"

#include <stdio.h>
#include <string.h>
#include <windows.h>

static void print_windows_error(const char *message, const char *path) {
    DWORD error_code;

    error_code = GetLastError();

    if (!is_empty_string(message) || !is_empty_string(path)) {
        fprintf(stderr, "Error: %s '%s' failed with Windows error code %lu.\n", message, path, (unsigned long)error_code);
    } else {
        fprintf(stderr, "Error: %s failed with Windows error code %lu.\n", message, (unsigned long)error_code);
    }
}

static CupError build_child_path(char *buffer, size_t size, const char *parent, const char *name) {
    CupError err;

    if (buffer == NULL || size == 0 || is_empty_string(parent) || is_empty_string(name)) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = checked_snprintf(buffer, size, "%s/%s", parent, name);
    return err;
}

static CupError build_search_pattern(char *buffer, size_t size, const char *path) {
    CupError err;

    if (buffer == NULL || size == 0 || is_empty_string(path)) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = checked_snprintf(buffer, size, "%s/*", path);
    return err;
}

static void fill_path_info_from_attributes(DWORD attributes, SystemPathInfo *info) {
    if (info == NULL) {
        return;
    }

    info->is_directory = ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0);
    info->is_regular_file = !info->is_directory;
}

CupError system_get_home_dir(char *buffer, size_t size) {
    DWORD result;

    if (buffer == NULL || size == 0) {
        return CUP_ERR_INVALID_INPUT;
    }

    result = GetEnvironmentVariableA("USERPROFILE", buffer, (DWORD)size);
    if (result == 0) {
        print_windows_error("could not read USERPROFILE", NULL);
        return CUP_ERR_FILESYSTEM;
    }

    if (result >= size) {
        fprintf(stderr, "Error: USERPROFILE path is too long.\n");
        return CUP_ERR_INVALID_INPUT;
    }

    return CUP_OK;
}

CupError system_get_process_id(char *buffer, size_t size) {
    CupError err;

    if (buffer == NULL || size == 0) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = checked_snprintf(buffer, size, "%lu", (unsigned long)GetCurrentProcessId());
    return err;
}

CupError system_make_directory(const char *path) {
    DWORD error_code;

    if (is_empty_string(path)) {
        return CUP_ERR_INVALID_INPUT;
    }

    if (CreateDirectoryA(path, NULL) == 0) {
        error_code = GetLastError();

        if (error_code == ERROR_ALREADY_EXISTS) {
            return CUP_OK;
        }

        print_windows_error("could not create directory", path);
        return CUP_ERR_FILESYSTEM;
    }

    return CUP_OK;
}

CupError system_remove_directory(const char *path) {
    if (is_empty_string(path)) {
        return CUP_ERR_INVALID_INPUT;
    }

    if (RemoveDirectoryA(path) == 0) {
        print_windows_error("could not remove directory", path);
        return CUP_ERR_FILESYSTEM;
    }

    return CUP_OK;
}

CupError system_rename_path(const char *source, const char *destination) {
    if (is_empty_string(source) || is_empty_string(destination)) {
        return CUP_ERR_INVALID_INPUT;
    }

    if (MoveFileExA(source, destination, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
        print_windows_error("could not rename path", source);
        return CUP_ERR_FILESYSTEM;
    }

    return CUP_OK;
}

CupError system_remove_file(const char *path) {
    DWORD attributes;

    if (is_empty_string(path)) {
        return CUP_ERR_INVALID_INPUT;
    }

    attributes = GetFileAttributesA(path);

    if (attributes != INVALID_FILE_ATTRIBUTES) {
        if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            fprintf(stderr, "Error: path '%s' is a directory, not a file.\n", path);
            return CUP_ERR_FILESYSTEM;
        }

        if ((attributes & FILE_ATTRIBUTE_READONLY) != 0) {
            if (!SetFileAttributesA(path, attributes & ~FILE_ATTRIBUTE_READONLY)) {
                fprintf(stderr, "Error: could not clear read-only attribute for '%s'.\n", path);
                return CUP_ERR_FILESYSTEM;
            }
        }
    }

    if (!DeleteFileA(path)) {
        fprintf(stderr, "Error: could not remove file '%s'.\n", path);
        return CUP_ERR_FILESYSTEM;
    }

    return CUP_OK;
}

CupError system_path_exists(const char *path, int *exists) {
    DWORD attributes;

    if (exists == NULL || is_empty_string(path)) {
        return CUP_ERR_INVALID_INPUT;
    }

    attributes = GetFileAttributesA(path);
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        DWORD error_code;

        error_code = GetLastError();
        if (error_code == ERROR_FILE_NOT_FOUND || error_code == ERROR_PATH_NOT_FOUND) {
            *exists = 0;
            return CUP_OK;
        }

        print_windows_error("could not inspect path", path);
        return CUP_ERR_FILESYSTEM;
    }

    *exists = 1;
    return CUP_OK;
}

CupError system_is_directory(const char *path, int *is_directory) {
    DWORD attributes;

    if (is_directory == NULL || is_empty_string(path)) {
        return CUP_ERR_INVALID_INPUT;
    }

    attributes = GetFileAttributesA(path);
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        DWORD error_code;

        error_code = GetLastError();
        if (error_code == ERROR_FILE_NOT_FOUND || error_code == ERROR_PATH_NOT_FOUND) {
            *is_directory = 0;
            return CUP_OK;
        }

        print_windows_error("could not inspect directory", path);
        return CUP_ERR_FILESYSTEM;
    }

    *is_directory = ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0);
    return CUP_OK;
}

CupError system_is_regular_file(const char *path, int *is_regular_file) {
    DWORD attributes;

    if (is_regular_file == NULL || is_empty_string(path)) {
        return CUP_ERR_INVALID_INPUT;
    }

    attributes = GetFileAttributesA(path);
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        DWORD error_code;

        error_code = GetLastError();
        if (error_code == ERROR_FILE_NOT_FOUND || error_code == ERROR_PATH_NOT_FOUND) {
            *is_regular_file = 0;
            return CUP_OK;
        }

        print_windows_error("could not inspect file", path);
        return CUP_ERR_FILESYSTEM;
    }

    *is_regular_file = ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0);
    return CUP_OK;
}

CupError system_file_size(const char *path, long long *file_size) {
    HANDLE file;
    LARGE_INTEGER size_value;

    if (file_size == NULL || is_empty_string(path)) {
        return CUP_ERR_INVALID_INPUT;
    }

    file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        print_windows_error("could not open file", path);
        return CUP_ERR_FILESYSTEM;
    }

    if (GetFileSizeEx(file, &size_value) == 0) {
        CloseHandle(file);
        print_windows_error("could not read file size", path);
        return CUP_ERR_FILESYSTEM;
    }

    CloseHandle(file);

    *file_size = (long long)size_value.QuadPart;
    return CUP_OK;
}

CupError system_walk_directory(const char *path, SystemDirectoryCallback callback, void *userdata) {
    CupError err;
    HANDLE find_handle;
    WIN32_FIND_DATAA find_data;
    char search_pattern[MAX_PATH_LEN];

    if (callback == NULL || is_empty_string(path)) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = build_search_pattern(search_pattern, sizeof(search_pattern), path);
    if (err != CUP_OK) {
        return err;
    }

    find_handle = FindFirstFileA(search_pattern, &find_data);
    if (find_handle == INVALID_HANDLE_VALUE) {
        DWORD error_code;

        error_code = GetLastError();

        if (error_code == ERROR_FILE_NOT_FOUND || error_code == ERROR_PATH_NOT_FOUND) {
            return CUP_OK;
        }

        print_windows_error("could not open directory", path);
        return CUP_ERR_FILESYSTEM;
    }

    do {
        char child_path[MAX_PATH_LEN];
        SystemPathInfo info;

        if (strcmp(find_data.cFileName, ".") == 0 || strcmp(find_data.cFileName, "..") == 0) {
            continue;
        }

        err = build_child_path(child_path, sizeof(child_path), path, find_data.cFileName);
        if (err != CUP_OK) {
            FindClose(find_handle);
            return err;
        }

        fill_path_info_from_attributes(find_data.dwFileAttributes, &info);

        if (info.is_directory) {
            err = system_walk_directory(child_path, callback, userdata);
            if (err != CUP_OK) {
                FindClose(find_handle);
                return err;
            }
        }

        err = callback(child_path, &info, userdata);
        if (err != CUP_OK) {
            FindClose(find_handle);
            return err;
        }
    } while (FindNextFileA(find_handle, &find_data) != 0);

    if (GetLastError() != ERROR_NO_MORE_FILES) {
        FindClose(find_handle);
        print_windows_error("could not continue directory traversal", path);
        return CUP_ERR_FILESYSTEM;
    }

    FindClose(find_handle);
    return CUP_OK;
}