#ifndef CUP_TEST_PLATFORM_H
#define CUP_TEST_PLATFORM_H

/* Test-only compatibility for deterministic filesystem and descriptor fixtures. */
#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CUP_TEST_TEMP_PATH_SIZE 1024

#ifdef _WIN32
#include <direct.h>
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#include <windows.h>

typedef struct _stat64 TestPlatformStat;

#define TEST_PLATFORM_STDOUT_FD 1
#define TEST_PLATFORM_STDERR_FD 2

static inline int test_set_home(const char *path) {
    return path == NULL ? EINVAL : _putenv_s("USERPROFILE", path);
}

static inline int test_mkdir(const char *path, int mode) {
    (void)mode;
    return _mkdir(path);
}

static inline int test_unlink(const char *path) {
    return _unlink(path);
}

/* Match system_replace_file semantics rather than the narrower Windows CRT rename(). */
static inline int test_replace_file(const char *source, const char *destination) {
    if (source == NULL || destination == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (MoveFileExA(source,
                    destination,
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        return 0;
    }
    errno = EIO;
    return -1;
}

static inline int test_rmdir(const char *path) {
    return _rmdir(path);
}

static inline int test_access_exists(const char *path) {
    return _access(path, 0) == 0;
}

static inline int test_stat_path(const char *path, TestPlatformStat *status) {
    return _stat64(path, status);
}

static inline int test_stat_is_regular(const TestPlatformStat *status) {
    return (status->st_mode & _S_IFMT) == _S_IFREG;
}

static inline int test_stat_is_directory(const TestPlatformStat *status) {
    return (status->st_mode & _S_IFMT) == _S_IFDIR;
}

static inline int test_dup_fd(int descriptor) {
    return _dup(descriptor);
}

static inline int test_dup2_fd(int source, int destination) {
    return _dup2(source, destination);
}

static inline int test_close_fd(int descriptor) {
    return _close(descriptor);
}

static inline int test_file_descriptor(FILE *file) {
    return _fileno(file);
}

static inline char *test_make_temp_directory(char *buffer,
                                             size_t size,
                                             const char *name) {
    const char *base = getenv("RUNNER_TEMP");
    int written;

    if (base == NULL || base[0] == '\0') {
        base = getenv("TEMP");
    }
    if (base == NULL || base[0] == '\0') {
        base = getenv("TMP");
    }
    if (base == NULL || base[0] == '\0') {
        base = ".";
    }

    written = snprintf(buffer, size, "%s/%s-XXXXXX", base, name);
    if (written < 0 || (size_t)written >= size) {
        return NULL;
    }
    if (_mktemp_s(buffer, size) != 0 || _mkdir(buffer) != 0) {
        return NULL;
    }
    return buffer;
}

static inline int test_create_temp_file(const char *directory,
                                        const char *prefix,
                                        char *path,
                                        size_t path_size,
                                        FILE **file) {
    int written;

    if (directory == NULL || prefix == NULL || path == NULL || path_size == 0 || file == NULL) {
        errno = EINVAL;
        return -1;
    }
    written = snprintf(path, path_size, "%s/%s-XXXXXX", directory, prefix);
    if (written < 0 || (size_t)written >= path_size || _mktemp_s(path, path_size) != 0) {
        return -1;
    }
    *file = fopen(path, "w+b");
    return *file == NULL ? -1 : 0;
}

static inline int test_remove_tree(const char *path) {
    WIN32_FIND_DATAA data;
    char pattern[CUP_TEST_TEMP_PATH_SIZE];
    HANDLE search;
    DWORD attributes;
    int written;

    attributes = GetFileAttributesA(path);
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        DWORD error = GetLastError();

        return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ? 0 : -1;
    }
    if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        return (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0
                   ? (RemoveDirectoryA(path) ? 0 : -1)
                   : (DeleteFileA(path) ? 0 : -1);
    }
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        if ((attributes & FILE_ATTRIBUTE_READONLY) != 0) {
            (void)SetFileAttributesA(path, attributes & ~FILE_ATTRIBUTE_READONLY);
        }
        return DeleteFileA(path) ? 0 : -1;
    }

    written = snprintf(pattern, sizeof(pattern), "%s\\*", path);
    if (written < 0 || (size_t)written >= sizeof(pattern)) {
        return -1;
    }
    search = FindFirstFileA(pattern, &data);
    if (search != INVALID_HANDLE_VALUE) {
        do {
            char child[CUP_TEST_TEMP_PATH_SIZE];

            if (strcmp(data.cFileName, ".") == 0 || strcmp(data.cFileName, "..") == 0) {
                continue;
            }
            written = snprintf(child, sizeof(child), "%s/%s", path, data.cFileName);
            if (written < 0 || (size_t)written >= sizeof(child) || test_remove_tree(child) != 0) {
                FindClose(search);
                return -1;
            }
        } while (FindNextFileA(search, &data));
        FindClose(search);
    } else if (GetLastError() != ERROR_FILE_NOT_FOUND) {
        return -1;
    }
    attributes = GetFileAttributesA(path);
    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_READONLY) != 0) {
        (void)SetFileAttributesA(path, attributes & ~FILE_ATTRIBUTE_READONLY);
    }
    return RemoveDirectoryA(path) ? 0 : -1;
}

static inline int test_sync_file(FILE *file) {
    if (file == NULL || fflush(file) != 0) {
        return -1;
    }
    return _commit(_fileno(file));
}
#else
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct stat TestPlatformStat;

#define TEST_PLATFORM_STDOUT_FD STDOUT_FILENO
#define TEST_PLATFORM_STDERR_FD STDERR_FILENO

static inline int test_set_home(const char *path) {
    return path == NULL ? EINVAL : setenv("HOME", path, 1);
}

static inline int test_mkdir(const char *path, int mode) {
    return mkdir(path, (mode_t)mode);
}

static inline int test_unlink(const char *path) {
    return unlink(path);
}

/* POSIX rename already provides the replacement contract required by the test doubles. */
static inline int test_replace_file(const char *source, const char *destination) {
    return rename(source, destination);
}

static inline int test_rmdir(const char *path) {
    return rmdir(path);
}

static inline int test_access_exists(const char *path) {
    return access(path, F_OK) == 0;
}

static inline int test_stat_path(const char *path, TestPlatformStat *status) {
    return lstat(path, status);
}

static inline int test_stat_is_regular(const TestPlatformStat *status) {
    return S_ISREG(status->st_mode);
}

static inline int test_stat_is_directory(const TestPlatformStat *status) {
    return S_ISDIR(status->st_mode);
}

static inline int test_dup_fd(int descriptor) {
    return dup(descriptor);
}

static inline int test_dup2_fd(int source, int destination) {
    return dup2(source, destination);
}

static inline int test_close_fd(int descriptor) {
    return close(descriptor);
}

static inline int test_file_descriptor(FILE *file) {
    return fileno(file);
}

static inline char *test_make_temp_directory(char *buffer,
                                             size_t size,
                                             const char *name) {
    const char *base = getenv("TMPDIR");
    int written;

    if (base == NULL || base[0] == '\0') {
        base = "/tmp";
    }
    written = snprintf(buffer, size, "%s/%s-XXXXXX", base, name);
    if (written < 0 || (size_t)written >= size) {
        return NULL;
    }
    return mkdtemp(buffer);
}

static inline int test_create_temp_file(const char *directory,
                                        const char *prefix,
                                        char *path,
                                        size_t path_size,
                                        FILE **file) {
    int descriptor;
    int written;

    if (directory == NULL || prefix == NULL || path == NULL || path_size == 0 || file == NULL) {
        errno = EINVAL;
        return -1;
    }
    written = snprintf(path, path_size, "%s/%s-XXXXXX", directory, prefix);
    if (written < 0 || (size_t)written >= path_size) {
        return -1;
    }
    descriptor = mkstemp(path);
    if (descriptor < 0) {
        return -1;
    }
    *file = fdopen(descriptor, "w+b");
    if (*file == NULL) {
        close(descriptor);
        unlink(path);
        return -1;
    }
    return 0;
}

static inline int test_remove_tree(const char *path) {
    DIR *directory = opendir(path);
    struct dirent *entry;

    if (directory == NULL) {
        return unlink(path) == 0 || errno == ENOENT ? 0 : -1;
    }
    while ((entry = readdir(directory)) != NULL) {
        char child[CUP_TEST_TEMP_PATH_SIZE];
        int written;

        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        written = snprintf(child, sizeof(child), "%s/%s", path, entry->d_name);
        if (written < 0 || (size_t)written >= sizeof(child) || test_remove_tree(child) != 0) {
            closedir(directory);
            return -1;
        }
    }
    if (closedir(directory) != 0) {
        return -1;
    }
    return rmdir(path);
}

static inline int test_sync_file(FILE *file) {
    if (file == NULL || fflush(file) != 0) {
        return -1;
    }
    return fsync(fileno(file));
}
#endif

#endif /* CUP_TEST_PLATFORM_H */
