#include "system.h"
#include "constants.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <dirent.h>

static CupError build_child_path(char *buffer, size_t size, const char *parent, const char *name) {
    CupError err;

    if (buffer == NULL || size == 0 || is_empty_string(parent) || is_empty_string(name)) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = checked_snprintf(buffer, size, "%s/%s", parent, name);
    return err;
}

static CupError system_get_path_info(const char *path, SystemPathInfo *info) {
    struct stat stat_info;

    if (info == NULL || is_empty_string(path)) {
        return CUP_ERR_INVALID_INPUT;
    }

    if (lstat(path, &stat_info) != 0) {
        fprintf(stderr, "Error: could not inspect path '%s': %s.\n", path, strerror(errno));
        return CUP_ERR_FILESYSTEM;
    }

    info->is_directory = S_ISDIR(stat_info.st_mode);
    info->is_regular_file = S_ISREG(stat_info.st_mode);

    return CUP_OK;
}

CupError system_get_home_dir(char *buffer, size_t size) {
    CupError err;
    const char *home;

    if (buffer == NULL || size == 0) {
        return CUP_ERR_INVALID_INPUT;
    }

    home = getenv("HOME");
    if (home == NULL || home[0] == '\0') {
        fprintf(stderr, "Error: HOME environment variable is not set.\n");
        return CUP_ERR_FILESYSTEM;
    }

    err = checked_snprintf(buffer, size, "%s", home);
    return err;
}

CupError system_get_process_id(char *buffer, size_t size) {
    CupError err;
    
    if (buffer == NULL || size == 0) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = checked_snprintf(buffer, size, "%ld", (long)getpid());
    return err;
}

CupError system_make_directory(const char *path) {
    if (is_empty_string(path)) {
        return CUP_ERR_INVALID_INPUT;
    }

    if (mkdir(path, 0755) != 0) {
        if (errno == EEXIST) {
            return CUP_OK;
        }

        fprintf(stderr, "Error: could not create directory '%s': %s.\n", path, strerror(errno));
        return CUP_ERR_FILESYSTEM;
    }

    return CUP_OK;
}

CupError system_remove_directory(const char *path) {
    if (is_empty_string(path)) {
        return CUP_ERR_INVALID_INPUT;
    }

    if (rmdir(path) != 0) {
        fprintf(stderr, "Error: could not remove directory '%s': %s.\n", path, strerror(errno));
        return CUP_ERR_FILESYSTEM;
    }

    return CUP_OK;
}

CupError system_rename_path(const char *source, const char *destination) {
    if (is_empty_string(source) || is_empty_string(destination)) {
        return CUP_ERR_INVALID_INPUT;
    }

    if (rename(source, destination) != 0) {
        fprintf(stderr, "Error: could not rename '%s' to '%s': %s.\n", source, destination, strerror(errno));
        return CUP_ERR_FILESYSTEM;
    }

    return CUP_OK;
}

CupError system_remove_file(const char *path) {
    if (is_empty_string(path)) {
        return CUP_ERR_INVALID_INPUT;
    }

    if (unlink(path) != 0) {
        fprintf(stderr, "Error: could not remove file '%s': %s.\n", path, strerror(errno));
        return CUP_ERR_FILESYSTEM;
    }

    return CUP_OK;
}

CupError system_path_exists(const char *path, int *exists) {
    struct stat info;

    if (exists == NULL || is_empty_string(path)) {
        return CUP_ERR_INVALID_INPUT;
    }

    if (lstat(path, &info) == 0) {
        *exists = 1;
        return CUP_OK;
    }

    if (errno == ENOENT) {
        *exists = 0;
        return CUP_OK;
    }

    fprintf(stderr, "Error: could not inspect path '%s': %s.\n", path, strerror(errno));
    return CUP_ERR_FILESYSTEM;
}

CupError system_is_directory(const char *path, int *is_directory) {
    struct stat info;

    if (is_directory == NULL || is_empty_string(path)) {
        return CUP_ERR_INVALID_INPUT;
    }

    if (lstat(path, &info) != 0) {
        if (errno == ENOENT) {
            *is_directory = 0;
            return CUP_OK;
        }

        fprintf(stderr, "Error: could not inspect directory '%s': %s.\n", path, strerror(errno));
        return CUP_ERR_FILESYSTEM;
    }

    *is_directory = S_ISDIR(info.st_mode);
    return CUP_OK;
}

CupError system_is_regular_file(const char *path, int *is_regular_file) {
    struct stat info;

    if (is_regular_file == NULL || is_empty_string(path)) {
        return CUP_ERR_INVALID_INPUT;
    }

    if (lstat(path, &info) != 0) {
        if (errno == ENOENT) {
            *is_regular_file = 0;
            return CUP_OK;
        }

        fprintf(stderr, "Error: could not inspect file '%s': %s.\n", path, strerror(errno));
        return CUP_ERR_FILESYSTEM;
    }

    *is_regular_file = S_ISREG(info.st_mode);
    return CUP_OK;
}

CupError system_file_size(const char *path, long long *file_size) {
    struct stat info;

    if (file_size == NULL || is_empty_string(path)) {
        return CUP_ERR_INVALID_INPUT;
    }

    if (lstat(path, &info) != 0) {
        fprintf(stderr, "Error: could not inspect file '%s': %s.\n", path, strerror(errno));
        return CUP_ERR_FILESYSTEM;
    }

    if (!S_ISREG(info.st_mode)) {
        fprintf(stderr, "Error: path '%s' is not a regular file.\n", path);
        return CUP_ERR_FILESYSTEM;
    }

    *file_size = (long long)info.st_size;
    return CUP_OK;
}

CupError system_walk_directory(const char *path, SystemDirectoryCallback callback, void *userdata) {
    DIR *directory;
    struct dirent *entry;
    CupError err;

    if (callback == NULL || is_empty_string(path)) {
        return CUP_ERR_INVALID_INPUT;
    }

    directory = opendir(path);
    if (directory == NULL) {
        fprintf(stderr, "Error: could not open directory '%s': %s.\n", path, strerror(errno));
        return CUP_ERR_FILESYSTEM;
    }

    while ((entry = readdir(directory)) != NULL) {
        char child_path[MAX_PATH_LEN];
        SystemPathInfo info;

        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        err = build_child_path(child_path, sizeof(child_path), path, entry->d_name);
        if (err != CUP_OK) {
            closedir(directory);
            return err;
        }

        err = system_get_path_info(child_path, &info);
        if (err != CUP_OK) {
            closedir(directory);
            return err;
        }

        if (info.is_directory) {
            err = system_walk_directory(child_path, callback, userdata);
            if (err != CUP_OK) {
                closedir(directory);
                return err;
            }
        }

        err = callback(child_path, &info, userdata);
        if (err != CUP_OK) {
            closedir(directory);
            return err;
        }
    }

    if (closedir(directory) != 0) {
        fprintf(stderr, "Error: could not close directory '%s': %s.\n", path, strerror(errno));
        return CUP_ERR_FILESYSTEM;
    }

    return CUP_OK;
}