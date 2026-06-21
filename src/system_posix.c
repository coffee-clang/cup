#include "system.h"

#include "constants.h"
#include "path.h"
#include "text.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif

#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0
#endif

// INTERNAL HELPERS
static CupError get_parent_path(const char *path, char *parent, size_t size) {
    char *slash;

    if (text_is_empty(path) || parent == NULL || size == 0 ||
        text_format(parent, size, "%s", path) != CUP_OK) {
        return CUP_ERR_INVALID_INPUT;
    }

    slash = strrchr(parent, '/');
    if (slash == NULL) {
        return text_format(parent, size, ".");
    }

    if (slash == parent) {
        slash[1] = '\0';
    } else {
        *slash = '\0';
    }

    return CUP_OK;
}

static CupError sync_directory(const char *path) {
    int fd;
    int sync_errno;

    if (text_is_empty(path)) {
        return CUP_ERR_INVALID_INPUT;
    }

#if defined(O_DIRECTORY)
    fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
#else
    fd = open(path, O_RDONLY | O_CLOEXEC);
#endif
    if (fd < 0) {
        return CUP_ERR_FILESYSTEM;
    }

    if (fsync(fd) != 0) {
        sync_errno = errno;
        close(fd);
        errno = sync_errno;
        return CUP_ERR_FILESYSTEM;
    }

    return close(fd) == 0 ? CUP_OK : CUP_ERR_FILESYSTEM;
}

static CupError sync_rename_directories(const char *source, const char *destination) {
    char source_parent[MAX_PATH_LEN];
    char destination_parent[MAX_PATH_LEN];
    CupError destination_err;
    CupError source_err = CUP_OK;

    if (get_parent_path(source, source_parent, sizeof(source_parent)) != CUP_OK ||
        get_parent_path(destination, destination_parent, sizeof(destination_parent)) != CUP_OK) {
        return CUP_ERR_FILESYSTEM;
    }

    destination_err = sync_directory(destination_parent);
    if (strcmp(source_parent, destination_parent) != 0) {
        source_err = sync_directory(source_parent);
    }

    return destination_err == CUP_OK && source_err == CUP_OK
        ? CUP_OK : CUP_ERR_FILESYSTEM;
}

static CupError open_regular_file_no_follow(const char *path, int flags, mode_t mode, int *fd) {
    struct stat info;
    int opened;

    if (text_is_empty(path) || fd == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    opened = open(path, flags | O_NOFOLLOW | O_CLOEXEC, mode);
    if (opened < 0) {
        return CUP_ERR_FILESYSTEM;
    }

    if (fstat(opened, &info) != 0 || !S_ISREG(info.st_mode)) {
        close(opened);
        return CUP_ERR_FILESYSTEM;
    }

    *fd = opened;
    return CUP_OK;
}

// PROCESS AND ENVIRONMENT
CupError system_get_home_dir(char *buffer, size_t size) {
    const char *home;

    if (buffer == NULL || size == 0) {
        return CUP_ERR_INVALID_INPUT;
    }

    home = getenv("HOME");
    if (text_is_empty(home)) {
        fprintf(stderr, "Error: HOME environment variable is not set.\n");
        return CUP_ERR_FILESYSTEM;
    }

    if (home[0] != '/') {
        fprintf(stderr, "Error: HOME must contain an absolute path.\n");
        return CUP_ERR_FILESYSTEM;
    }

    return text_format(buffer, size, "%s", home);
}

unsigned long system_get_process_id(void) {
    return (unsigned long)getpid();
}

CupError system_start_uninstall(const char *cup_root,
    const char *uninstall_script, unsigned long parent_pid) {
    CupError err;
    char tmp_script[MAX_PATH_LEN];
    FILE *file = NULL;
    pid_t pid;
    char parent_pid_text[32];

    if (text_is_empty(cup_root) || text_is_empty(uninstall_script) ||
        parent_pid == 0 || text_format(parent_pid_text,
            sizeof(parent_pid_text), "%lu", parent_pid) != CUP_OK) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = system_create_temp_file("/tmp", "cup-uninstall",
        tmp_script, sizeof(tmp_script), &file);
    if (err != CUP_OK) {
        fprintf(stderr, "Error: could not create temporary uninstall script.\n");
        return err;
    }

    if (fclose(file) != 0) {
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
        execl("/bin/sh", "sh", tmp_script, cup_root, tmp_script,
            parent_pid_text, (char *)NULL);
        _exit(127);
    }

    return CUP_OK;
}

// FILES AND DIRECTORIES
CupError system_make_directory(const char *path) {
    SystemPathKind info;

    if (text_is_empty(path)) {
        return CUP_ERR_INVALID_INPUT;
    }

    if (mkdir(path, 0755) == 0) {
        return CUP_OK;
    }

    if (errno != EEXIST || system_get_path_kind(path, &info) != CUP_OK ||
        info != SYSTEM_PATH_DIRECTORY) {
        fprintf(stderr, "Error: could not create directory '%s': %s.\n", path, strerror(errno));
        return CUP_ERR_FILESYSTEM;
    }

    return CUP_OK;
}

CupError system_remove_directory(const char *path) {
    SystemPathKind info;

    if (text_is_empty(path)) {
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

    if (rmdir(path) != 0) {
        fprintf(stderr, "Error: could not remove directory '%s': %s.\n", path, strerror(errno));
        return CUP_ERR_FILESYSTEM;
    }
    return CUP_OK;
}

CupError system_move_path(const char *source, const char *destination,
    SystemCommitState *commit_state) {
    SystemPathKind info;

    if (text_is_empty(source) || text_is_empty(destination) || commit_state == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    *commit_state = SYSTEM_COMMIT_NOT_APPLIED;

    if (system_get_path_kind(destination, &info) != CUP_OK) {
        return CUP_ERR_FILESYSTEM;
    }
    if (info != SYSTEM_PATH_MISSING) {
        fprintf(stderr, "Error: destination '%s' already exists.\n", destination);
        return CUP_ERR_FILESYSTEM;
    }

    if (rename(source, destination) != 0) {
        fprintf(stderr, "Error: could not move '%s' to '%s': %s.\n",
            source, destination, strerror(errno));
        return CUP_ERR_FILESYSTEM;
    }

    *commit_state = SYSTEM_COMMIT_APPLIED;
    if (sync_rename_directories(source, destination) != CUP_OK) {
        return CUP_ERR_FILESYSTEM;
    }

    *commit_state = SYSTEM_COMMIT_DURABLE;
    return CUP_OK;
}

CupError system_replace_file(const char *source, const char *destination,
    SystemCommitState *commit_state) {
    if (text_is_empty(source) || text_is_empty(destination) || commit_state == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    *commit_state = SYSTEM_COMMIT_NOT_APPLIED;

    if (rename(source, destination) != 0) {
        fprintf(stderr, "Error: could not replace '%s' with '%s': %s.\n",
            destination, source, strerror(errno));
        return CUP_ERR_FILESYSTEM;
    }

    *commit_state = SYSTEM_COMMIT_APPLIED;
    if (sync_rename_directories(source, destination) != CUP_OK) {
        return CUP_ERR_FILESYSTEM;
    }

    *commit_state = SYSTEM_COMMIT_DURABLE;
    return CUP_OK;
}

CupError system_remove_file(const char *path) {
    SystemPathKind info;

    if (text_is_empty(path)) {
        return CUP_ERR_INVALID_INPUT;
    }

    if (system_get_path_kind(path, &info) != CUP_OK) {
        return CUP_ERR_FILESYSTEM;
    }
    if (info == SYSTEM_PATH_MISSING) {
        return CUP_OK;
    }
    if (info == SYSTEM_PATH_DIRECTORY) {
        return CUP_ERR_FILESYSTEM;
    }

    if (unlink(path) != 0) {
        fprintf(stderr, "Error: could not remove file '%s': %s.\n", path, strerror(errno));
        return CUP_ERR_FILESYSTEM;
    }
    return CUP_OK;
}

CupError system_copy_file(const char *source_path, const char *destination_path) {
    FILE *source = NULL;
    FILE *destination = NULL;
    unsigned char buffer[8192];
    size_t count;
    int source_fd = -1;
    int destination_fd = -1;
    int failed = 0;

    if (text_is_empty(source_path) || text_is_empty(destination_path)) {
        return CUP_ERR_INVALID_INPUT;
    }

    if (open_regular_file_no_follow(source_path, O_RDONLY, 0, &source_fd) != CUP_OK) {
        return CUP_ERR_FILESYSTEM;
    }
    source = fdopen(source_fd, "rb");
    if (source == NULL) {
        close(source_fd);
        return CUP_ERR_FILESYSTEM;
    }

    if (open_regular_file_no_follow(destination_path,
        O_WRONLY | O_CREAT | O_TRUNC, 0600, &destination_fd) != CUP_OK) {
        fclose(source);
        return CUP_ERR_FILESYSTEM;
    }
    destination = fdopen(destination_fd, "wb");
    if (destination == NULL) {
        close(destination_fd);
        fclose(source);
        return CUP_ERR_FILESYSTEM;
    }

    while ((count = fread(buffer, 1, sizeof(buffer), source)) > 0) {
        if (fwrite(buffer, 1, count, destination) != count) {
            failed = 1;
            break;
        }
    }

    if (ferror(source) != 0) {
        failed = 1;
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
        system_remove_file(destination_path);
        return CUP_ERR_FILESYSTEM;
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

CupError system_sync_parent_directory(const char *path) {
    char parent[MAX_PATH_LEN];

    if (get_parent_path(path, parent, sizeof(parent)) != CUP_OK) {
        return CUP_ERR_INVALID_INPUT;
    }
    return sync_directory(parent);
}

// TEMPORARY OBJECTS
CupError system_create_file_exclusive(const char *path, FILE **file) {
    int fd;

    if (text_is_empty(path) || file == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    *file = NULL;

    fd = open(path, O_CREAT | O_EXCL | O_RDWR | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) {
        return errno == EEXIST ? CUP_ERR_LOCK : CUP_ERR_FILESYSTEM;
    }

    *file = fdopen(fd, "w+b");
    if (*file == NULL) {
        close(fd);
        unlink(path);
        return CUP_ERR_FILESYSTEM;
    }
    return CUP_OK;
}

CupError system_create_temp_file(const char *directory, const char *prefix,
    char *path, size_t path_size, FILE **file) {
    int fd;

    if (text_is_empty(directory) || text_is_empty(prefix) || path == NULL ||
        path_size == 0 || file == NULL ||
        text_format(path, path_size, "%s/%s-XXXXXX", directory, prefix) != CUP_OK) {
        return CUP_ERR_INVALID_INPUT;
    }

    *file = NULL;
    fd = mkstemp(path);
    if (fd < 0) {
        return CUP_ERR_TEMPORARY;
    }

    if (fchmod(fd, 0600) != 0) {
        close(fd);
        unlink(path);
        return CUP_ERR_TEMPORARY;
    }

    *file = fdopen(fd, "w+b");
    if (*file == NULL) {
        close(fd);
        unlink(path);
        return CUP_ERR_TEMPORARY;
    }

    return CUP_OK;
}

CupError system_create_temp_directory(const char *directory, const char *prefix,
    char *path, size_t path_size) {
    if (text_is_empty(directory) || text_is_empty(prefix) || path == NULL ||
        path_size == 0 ||
        text_format(path, path_size, "%s/%s-XXXXXX", directory, prefix) != CUP_OK) {
        return CUP_ERR_INVALID_INPUT;
    }

    if (mkdtemp(path) == NULL || chmod(path, 0700) != 0) {
        if (path[0] != '\0') {
            rmdir(path);
        }
        return CUP_ERR_TEMPORARY;
    }

    return CUP_OK;
}

CupError system_make_unique_temp_path(const char *directory, const char *prefix,
    char *path, size_t path_size) {
    FILE *file = NULL;
    CupError err;

    err = system_create_temp_file(directory, prefix, path, path_size, &file);
    if (err != CUP_OK) {
        return err;
    }

    {
        int close_failed = fclose(file) != 0;
        int remove_failed = unlink(path) != 0;

        return close_failed || remove_failed ? CUP_ERR_TEMPORARY : CUP_OK;
    }
}

// PATH INSPECTION
CupError system_get_path_kind(const char *path, SystemPathKind *path_kind) {
    struct stat stat_info;

    if (path_kind == NULL || text_is_empty(path)) {
        return CUP_ERR_INVALID_INPUT;
    }

    *path_kind = SYSTEM_PATH_MISSING;
    if (lstat(path, &stat_info) != 0) {
        if (errno == ENOENT || errno == ENOTDIR) {
            return CUP_OK;
        }
        fprintf(stderr, "Error: could not inspect path '%s': %s.\n",
            path, strerror(errno));
        return CUP_ERR_FILESYSTEM;
    }

    if (S_ISLNK(stat_info.st_mode)) {
        *path_kind = SYSTEM_PATH_LINK;
    } else if (S_ISDIR(stat_info.st_mode)) {
        *path_kind = SYSTEM_PATH_DIRECTORY;
    } else if (S_ISREG(stat_info.st_mode)) {
        *path_kind = SYSTEM_PATH_REGULAR_FILE;
    } else {
        *path_kind = SYSTEM_PATH_OTHER;
    }

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
    struct stat info;

    if (file_size == NULL || text_is_empty(path)) {
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
    SystemPathKind info;
    CupError err;

    if (is_executable == NULL || text_is_empty(path)) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = system_get_path_kind(path, &info);
    if (err != CUP_OK) {
        return err;
    }

    *is_executable = info == SYSTEM_PATH_REGULAR_FILE && access(path, X_OK) == 0;
    return CUP_OK;
}

CupError system_is_read_only(const char *path, int *is_read_only) {
    struct stat info;

    if (is_read_only == NULL || text_is_empty(path)) {
        return CUP_ERR_INVALID_INPUT;
    }
    if (lstat(path, &info) != 0 || S_ISLNK(info.st_mode)) {
        return CUP_ERR_FILESYSTEM;
    }
    *is_read_only = (info.st_mode & (S_IWUSR | S_IWGRP | S_IWOTH)) == 0;
    return CUP_OK;
}

CupError system_set_read_only(const char *path, int read_only) {
    struct stat info;
    mode_t mode;

    if (text_is_empty(path)) {
        return CUP_ERR_INVALID_INPUT;
    }
    if (lstat(path, &info) != 0 || S_ISLNK(info.st_mode)) {
        return CUP_ERR_FILESYSTEM;
    }

    mode = info.st_mode;
    if (read_only) {
        mode &= ~(S_IWUSR | S_IWGRP | S_IWOTH);
    } else {
        mode |= S_IWUSR;
    }

    return chmod(path, mode) == 0 ? CUP_OK : CUP_ERR_FILESYSTEM;
}

CupError system_set_executable(const char *path, int executable) {
    struct stat info;
    mode_t mode;

    if (text_is_empty(path)) {
        return CUP_ERR_INVALID_INPUT;
    }
    if (lstat(path, &info) != 0 || !S_ISREG(info.st_mode)) {
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

    return chmod(path, mode) == 0 ? CUP_OK : CUP_ERR_FILESYSTEM;
}

// DIRECTORY TRAVERSAL
CupError system_list_directory(const char *path, SystemDirectoryCallback callback, void *userdata) {
    DIR *directory;
    struct dirent *entry;
    CupError err;
    int read_error;
    SystemPathKind root_info;

    if (callback == NULL || text_is_empty(path)) {
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

    directory = opendir(path);
    if (directory == NULL) {
        return CUP_ERR_FILESYSTEM;
    }

    errno = 0;
    while ((entry = readdir(directory)) != NULL) {
        char child_path[MAX_PATH_LEN];
        SystemPathKind info;

        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        err = path_join(child_path, sizeof(child_path), path, entry->d_name);
        if (err != CUP_OK || system_get_path_kind(child_path, &info) != CUP_OK ||
            info == SYSTEM_PATH_MISSING) {
            closedir(directory);
            return CUP_ERR_FILESYSTEM;
        }

        err = callback(child_path, info, userdata);
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

static CupError walk_directory_entry(const char *path,
    SystemPathKind path_kind, void *userdata) {
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
    struct flock operation;
    struct stat info;
    int fd;

    if (lock == NULL || text_is_empty(path)) {
        return CUP_ERR_INVALID_INPUT;
    }

    memset(lock, 0, sizeof(*lock));
    fd = open(path, O_CREAT | O_RDWR | O_NOFOLLOW | O_CLOEXEC, 0644);
    if (fd < 0) {
        return CUP_ERR_FILESYSTEM;
    }
    if (fstat(fd, &info) != 0 || !S_ISREG(info.st_mode)) {
        close(fd);
        return CUP_ERR_FILESYSTEM;
    }

    memset(&operation, 0, sizeof(operation));
    operation.l_type = mode == SYSTEM_LOCK_EXCLUSIVE ? F_WRLCK : F_RDLCK;
    operation.l_whence = SEEK_SET;

    if (fcntl(fd, F_SETLK, &operation) != 0) {
        int lock_errno = errno;
        close(fd);
        if (lock_errno == EACCES || lock_errno == EAGAIN) {
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
