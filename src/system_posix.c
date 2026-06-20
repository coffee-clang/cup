#include "system.h"

#include "constants.h"
#include "path.h"
#include "util.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

// INTERNAL HELPERS
static CupError get_path_info(const char *path, SystemPathInfo *info) {
    struct stat stat_info;

    if (info == NULL || is_empty_string(path)) {
        return CUP_ERR_INVALID_INPUT;
    }

    if (lstat(path, &stat_info) != 0) {
        fprintf(stderr, "Error: could not inspect path '%s': %s.\n", path, strerror(errno));
        return CUP_ERR_FILESYSTEM;
    }

    info->is_directory = S_ISDIR(stat_info.st_mode);
    info->is_reparse_point = S_ISLNK(stat_info.st_mode);
    info->is_regular_file = S_ISREG(stat_info.st_mode);
    return CUP_OK;
}

static CupError sync_parent_directory(const char *path) {
    char parent[MAX_PATH_LEN];
    char *slash;
    int fd;

    if (checked_snprintf(parent, sizeof(parent), "%s", path) != CUP_OK) {
        return CUP_ERR_FILESYSTEM;
    }

    slash = strrchr(parent, '/');
    if (slash == NULL) {
        return CUP_OK;
    }

    if (slash == parent) {
        slash[1] = '\0';
    } else {
        *slash = '\0';
    }

    fd = open(parent, O_RDONLY);
    if (fd < 0) {
        return CUP_ERR_FILESYSTEM;
    }
    if (fsync(fd) != 0) {
        close(fd);
        return CUP_ERR_FILESYSTEM;
    }
    return close(fd) == 0 ? CUP_OK : CUP_ERR_FILESYSTEM;
}

// PROCESS AND ENVIRONMENT
CupError system_get_home_dir(char *buffer, size_t size) {
    const char *home;

    if (buffer == NULL || size == 0) {
        return CUP_ERR_INVALID_INPUT;
    }

    home = getenv("HOME");
    if (is_empty_string(home)) {
        fprintf(stderr, "Error: HOME environment variable is not set.\n");
        return CUP_ERR_FILESYSTEM;
    }

    return checked_snprintf(buffer, size, "%s", home);
}

CupError system_get_process_id(char *buffer, size_t size) {
    if (buffer == NULL || size == 0) {
        return CUP_ERR_INVALID_INPUT;
    }
    return checked_snprintf(buffer, size, "%ld", (long)getpid());
}

CupError system_start_uninstall(const char *cup_root, const char *uninstall_script) {
    CupError err;
    char tmp_script[MAX_PATH_LEN];
    int fd;
    pid_t pid;

    if (is_empty_string(cup_root) || is_empty_string(uninstall_script)) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = checked_snprintf(tmp_script, sizeof(tmp_script), "/tmp/cup-uninstall-XXXXXX");
    if (err != CUP_OK) {
        return err;
    }

    fd = mkstemp(tmp_script);
    if (fd < 0) {
        fprintf(stderr, "Error: could not create temporary uninstall script: %s.\n", strerror(errno));
        return CUP_ERR_FILESYSTEM;
    }
    if (close(fd) != 0) {
        system_remove_file(tmp_script);
        return CUP_ERR_FILESYSTEM;
    }

    err = system_copy_file(uninstall_script, tmp_script);
    if (err != CUP_OK) {
        system_remove_file(tmp_script);
        return err;
    }

    err = system_set_executable(tmp_script, 1);
    if (err != CUP_OK) {
        system_remove_file(tmp_script);
        return err;
    }

    pid = fork();
    if (pid < 0) {
        system_remove_file(tmp_script);
        return CUP_ERR_FILESYSTEM;
    }

    if (pid == 0) {
        execl("/bin/sh", "sh", tmp_script, cup_root, tmp_script, (char *)NULL);
        _exit(127);
    }

    return CUP_OK;
}

// FILES AND DIRECTORIES
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
        if (errno == ENOENT) {
            return CUP_OK;
        }
        fprintf(stderr, "Error: could not remove directory '%s': %s.\n", path, strerror(errno));
        return CUP_ERR_FILESYSTEM;
    }
    return CUP_OK;
}

CupError system_move_path(const char *source, const char *destination) {
    CupError err;
    int exists;

    if (is_empty_string(source) || is_empty_string(destination)) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = system_path_exists(destination, &exists);
    if (err != CUP_OK) {
        return err;
    }
    if (exists) {
        fprintf(stderr, "Error: destination '%s' already exists.\n", destination);
        return CUP_ERR_FILESYSTEM;
    }

    if (rename(source, destination) != 0) {
        fprintf(stderr, "Error: could not move '%s' to '%s': %s.\n", source, destination, strerror(errno));
        return CUP_ERR_FILESYSTEM;
    }
    return sync_parent_directory(destination);
}

CupError system_replace_file(const char *source, const char *destination) {
    if (is_empty_string(source) || is_empty_string(destination)) {
        return CUP_ERR_INVALID_INPUT;
    }
    if (rename(source, destination) != 0) {
        fprintf(stderr, "Error: could not replace '%s' with '%s': %s.\n", destination, source, strerror(errno));
        return CUP_ERR_FILESYSTEM;
    }
    return sync_parent_directory(destination);
}

CupError system_remove_file(const char *path) {
    if (is_empty_string(path)) {
        return CUP_ERR_INVALID_INPUT;
    }
    if (unlink(path) != 0) {
        if (errno == ENOENT) {
            return CUP_OK;
        }
        fprintf(stderr, "Error: could not remove file '%s': %s.\n", path, strerror(errno));
        return CUP_ERR_FILESYSTEM;
    }
    return CUP_OK;
}

CupError system_copy_file(const char *source_path, const char *destination_path) {
    FILE *source;
    FILE *destination;
    unsigned char buffer[8192];
    size_t count;

    if (is_empty_string(source_path) || is_empty_string(destination_path)) {
        return CUP_ERR_INVALID_INPUT;
    }
    source = fopen(source_path, "rb");
    if (source == NULL) {
        return CUP_ERR_FILESYSTEM;
    }
    destination = fopen(destination_path, "wb");
    if (destination == NULL) {
        fclose(source);
        return CUP_ERR_FILESYSTEM;
    }
    while ((count = fread(buffer, 1, sizeof(buffer), source)) > 0) {
        if (fwrite(buffer, 1, count, destination) != count) {
            fclose(source);
            fclose(destination);
            system_remove_file(destination_path);
            return CUP_ERR_FILESYSTEM;
        }
    }

    {
        int failed = ferror(source) != 0;

        if (fclose(source) != 0) {
            failed = 1;
        }
        if (system_sync_file(destination) != CUP_OK) {
            failed = 1;
        }
        if (fclose(destination) != 0) {
            failed = 1;
        }
        if (failed) {
            system_remove_file(destination_path);
            return CUP_ERR_FILESYSTEM;
        }
    }
    return CUP_OK;
}

CupError system_sync_file(FILE *file) {
    if (file == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    if (fflush(file) != 0 || fsync(fileno(file)) != 0) {
        return CUP_ERR_FILESYSTEM;
    }
    return CUP_OK;
}

// PATH INSPECTION
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
    if (lstat(path, &info) != 0 || !S_ISREG(info.st_mode)) {
        return CUP_ERR_FILESYSTEM;
    }
    *file_size = (long long)info.st_size;
    return CUP_OK;
}

// PERMISSIONS
CupError system_is_executable(const char *path, int *is_executable) {
    struct stat info;

    if (is_executable == NULL || is_empty_string(path)) {
        return CUP_ERR_INVALID_INPUT;
    }
    if (stat(path, &info) != 0) {
        return CUP_ERR_FILESYSTEM;
    }
    *is_executable = S_ISREG(info.st_mode) && (info.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH)) != 0;
    return CUP_OK;
}

CupError system_is_read_only(const char *path, int *is_read_only) {
    struct stat info;

    if (is_read_only == NULL || is_empty_string(path)) {
        return CUP_ERR_INVALID_INPUT;
    }
    if (stat(path, &info) != 0) {
        return CUP_ERR_FILESYSTEM;
    }
    *is_read_only = (info.st_mode & (S_IWUSR | S_IWGRP | S_IWOTH)) == 0;
    return CUP_OK;
}

CupError system_set_read_only(const char *path, int read_only) {
    struct stat info;
    mode_t mode;

    if (is_empty_string(path)) {
        return CUP_ERR_INVALID_INPUT;
    }
    if (stat(path, &info) != 0) {
        return CUP_ERR_FILESYSTEM;
    }

    mode = info.st_mode;
    if (read_only) {
        mode &= ~(S_IWUSR | S_IWGRP | S_IWOTH);
    } else {
        mode |= S_IWUSR;
    }

    if (chmod(path, mode) != 0) {
        return CUP_ERR_FILESYSTEM;
    }
    return CUP_OK;
}

CupError system_set_executable(const char *path, int executable) {
    struct stat info;
    mode_t mode;

    if (is_empty_string(path)) {
        return CUP_ERR_INVALID_INPUT;
    }
    if (stat(path, &info) != 0) {
        return CUP_ERR_FILESYSTEM;
    }
    mode = info.st_mode;
    if (executable) {
        mode |= S_IXUSR;
        if (mode & S_IRGRP) {
            mode |= S_IXGRP;
        }
        if (mode & S_IROTH) {
            mode |= S_IXOTH;
        }
    } else {
        mode &= ~(S_IXUSR | S_IXGRP | S_IXOTH);
    }
    if (chmod(path, mode) != 0) {
        return CUP_ERR_FILESYSTEM;
    }
    return CUP_OK;
}

// DIRECTORY TRAVERSAL
CupError system_list_directory(const char *path, SystemDirectoryCallback callback, void *userdata) {
    DIR *directory;
    struct dirent *entry;
    CupError err;
    int read_error;

    if (callback == NULL || is_empty_string(path)) {
        return CUP_ERR_INVALID_INPUT;
    }

    directory = opendir(path);
    if (directory == NULL) {
        if (errno == ENOENT) {
            return CUP_OK;
        }
        return CUP_ERR_FILESYSTEM;
    }

    errno = 0;
    while ((entry = readdir(directory)) != NULL) {
        char child_path[MAX_PATH_LEN];
        SystemPathInfo info;

        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        err = path_join(child_path, sizeof(child_path), path, entry->d_name);
        if (err != CUP_OK || get_path_info(child_path, &info) != CUP_OK) {
            closedir(directory);
            return CUP_ERR_FILESYSTEM;
        }

        err = callback(child_path, &info, userdata);
        if (err != CUP_OK) {
            closedir(directory);
            return err;
        }
        errno = 0;
    }

    read_error = errno;
    if (closedir(directory) != 0 || read_error != 0) {
        return CUP_ERR_FILESYSTEM;
    }
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
    struct flock operation;
    int fd;

    if (lock == NULL || is_empty_string(path)) {
        return CUP_ERR_INVALID_INPUT;
    }

    memset(lock, 0, sizeof(*lock));
    fd = open(path, O_CREAT | O_RDWR, 0644);
    if (fd < 0) {
        return CUP_ERR_FILESYSTEM;
    }

    memset(&operation, 0, sizeof(operation));
    operation.l_type = mode == SYSTEM_LOCK_EXCLUSIVE ? F_WRLCK : F_RDLCK;
    operation.l_whence = SEEK_SET;

    if (fcntl(fd, F_SETLK, &operation) != 0) {
        close(fd);
        if (errno == EACCES || errno == EAGAIN) {
            return CUP_ERR_LOCK;
        }
        return CUP_ERR_FILESYSTEM;
    }

    lock->handle = fd;
    lock->active = 1;
    return CUP_OK;
}

void system_lock_release(SystemLock *lock) {
    struct flock operation;

    if (lock == NULL || !lock->active) {
        return;
    }

    memset(&operation, 0, sizeof(operation));
    operation.l_type = F_UNLCK;
    operation.l_whence = SEEK_SET;
    fcntl((int)lock->handle, F_SETLK, &operation);
    close((int)lock->handle);
    lock->handle = -1;
    lock->active = 0;
}
