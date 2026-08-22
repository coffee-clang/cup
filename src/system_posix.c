#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

/*
 * Implements the complete system.h contract with POSIX primitives, including path inspection
 * without link following, locks, durable replacement and detached helpers.
 */

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
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#if defined(__linux__)
#include <sys/syscall.h>
#endif
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

#ifndef O_CLOEXEC
#error "CUP requires O_CLOEXEC on supported POSIX platforms"
#endif

#ifndef O_NOFOLLOW
#error "CUP requires O_NOFOLLOW on supported POSIX platforms"
#endif

#ifdef CUP_SYSTEM_TESTING
static void system_test_pause(const char *point) {
    const char *expected = getenv("CUP_PATH_OPS_TEST_POINT");
    const char *ready = getenv("CUP_PATH_OPS_TEST_READY");
    const char *resume = getenv("CUP_PATH_OPS_TEST_CONTINUE");
    struct timespec delay = {0, 10000000L};
    unsigned int attempt;
    int descriptor;

    if (expected == NULL || strcmp(expected, point) != 0) {
        return;
    }
    if (ready == NULL || resume == NULL) {
        return;
    }

    descriptor = open(ready, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (descriptor < 0) {
        return;
    }
    close(descriptor);

    for (attempt = 0; attempt < 3000; ++attempt) {
        if (access(resume, F_OK) == 0) {
            return;
        }
        if (errno != ENOENT) {
            return;
        }
        nanosleep(&delay, NULL);
    }
}

static int system_test_crosses_boundary(const char *name, dev_t observed, dev_t root) {
    const char *forced = getenv("CUP_PATH_OPS_TEST_BOUNDARY_NAME");

    if (forced != NULL && name != NULL && strcmp(forced, name) == 0) {
        return 1;
    }
    return observed != root;
}
#else
static void system_test_pause(const char *point) {
    (void)point;
}

static int system_test_crosses_boundary(const char *name, dev_t observed, dev_t root) {
    (void)name;
    return observed != root;
}
#endif

/* Descriptor-level helpers shared by no-follow path operations. */
static CupError open_directory_path_no_follow(const char *path, int *descriptor);

static CupError sync_directory(const char *path) {
    int fd = -1;
    int sync_errno;
    CupError err;

    if (text_is_empty(path)) {
        return CUP_ERR_INVALID_INPUT;
    }
    err = open_directory_path_no_follow(path, &fd);
    if (err != CUP_OK) {
        return err;
    }

    if (fsync(fd) != 0) {
        sync_errno = errno;
        close(fd);
        errno = sync_errno;
        return CUP_ERR_FILESYSTEM;
    }

    (void)close(fd);
    return CUP_OK;
}

static CupError split_parent_entry(
    const char *path, char *parent, size_t parent_size, char *entry, size_t entry_size);

static SystemPathKind path_kind_from_mode(mode_t mode) {
    if (S_ISREG(mode)) {
        return SYSTEM_PATH_REGULAR_FILE;
    }
    if (S_ISDIR(mode)) {
        return SYSTEM_PATH_DIRECTORY;
    }
    if (S_ISLNK(mode)) {
        return SYSTEM_PATH_LINK;
    }
    return SYSTEM_PATH_OTHER;
}

static void identity_from_stat(const struct stat *info, SystemPathIdentity *identity) {
    identity->volume = (uint64_t)info->st_dev;
    identity->object = (uint64_t)info->st_ino;
    identity->object_high = 0;
    identity->kind = path_kind_from_mode(info->st_mode);
    identity->valid = 1;
}

static int stat_identity_equal(const struct stat *left, const struct stat *right) {
    return left->st_dev == right->st_dev && left->st_ino == right->st_ino &&
           (left->st_mode & S_IFMT) == (right->st_mode & S_IFMT);
}

static CupError open_directory_path_no_follow_options(const char *path,
                                                      int create,
                                                      int *descriptor,
                                                      int *missing) {
    char copy[MAX_PATH_LEN];
    char *save = NULL;
    char *component;
    int current;
    int path_missing = 0;

    if (text_is_empty(path) || descriptor == NULL || (create != 0 && create != 1)) {
        return CUP_ERR_INVALID_INPUT;
    }
    if (strlen(path) >= sizeof(copy)) {
        return CUP_ERR_BUFFER_TOO_SMALL;
    }
    *descriptor = -1;
    if (missing != NULL) {
        *missing = 0;
    }

    memcpy(copy, path, strlen(path) + 1);
    if (strcmp(path, ".") == 0) {
        current = open(".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (current < 0) {
            return CUP_ERR_FILESYSTEM;
        }
        *descriptor = current;
        return CUP_OK;
    }

    current = open(path[0] == '/' ? "/" : ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (current < 0) {
        return CUP_ERR_FILESYSTEM;
    }

    component = strtok_r(copy + (path[0] == '/' ? 1 : 0), "/", &save);
    while (component != NULL) {
        int created = 0;
        int next;

        if (component[0] == '\0' || strcmp(component, ".") == 0 ||
            strcmp(component, "..") == 0) {
            if (current >= 0) {
                close(current);
            }
            return CUP_ERR_INVALID_INPUT;
        }
        if (path_missing) {
            component = strtok_r(NULL, "/", &save);
            continue;
        }

        next = openat(current,
                      component,
                      O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (next < 0 && errno == ENOENT && create) {
            system_test_pause("before-mkdir-component");
            if (mkdirat(current, component, 0755) == 0) {
                created = 1;
            } else if (errno != EEXIST) {
                close(current);
                return CUP_ERR_FILESYSTEM;
            }

            next = openat(current,
                          component,
                          O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (next >= 0 && created && fchmod(next, 0755) != 0) {
                close(next);
                close(current);
                return CUP_ERR_FILESYSTEM;
            }
        }
        if (next < 0) {
            int open_error = errno;

            close(current);
            current = -1;
            if (missing != NULL && open_error == ENOENT) {
                *missing = 1;
                path_missing = 1;
                component = strtok_r(NULL, "/", &save);
                continue;
            }
            errno = open_error;
            return CUP_ERR_FILESYSTEM;
        }

        close(current);
        current = next;
        component = strtok_r(NULL, "/", &save);
    }

    *descriptor = current;
    return CUP_OK;
}

static CupError open_directory_path_no_follow_status(const char *path,
                                                     int *descriptor,
                                                     int *missing) {
    return open_directory_path_no_follow_options(path, 0, descriptor, missing);
}

static CupError open_directory_path_no_follow(const char *path, int *descriptor) {
    return open_directory_path_no_follow_status(path, descriptor, NULL);
}

static CupError open_parent_no_follow_status(const char *path,
                                             int *parent_fd,
                                             char *entry,
                                             size_t entry_size,
                                             int *missing) {
    char parent[MAX_PATH_LEN];
    CupError err;

    err = split_parent_entry(path, parent, sizeof(parent), entry, entry_size);
    if (err != CUP_OK) {
        return err;
    }
    err = open_directory_path_no_follow_status(parent, parent_fd, missing);
    if (err == CUP_OK && (missing == NULL || !*missing) && *parent_fd < 0) {
        return CUP_ERR_FILESYSTEM;
    }
    return err;
}

static CupError open_parent_no_follow(const char *path,
                                      int *parent_fd,
                                      char *entry,
                                      size_t entry_size) {
    return open_parent_no_follow_status(
        path, parent_fd, entry, entry_size, NULL);
}

static CupError sync_directory_fd(int descriptor) {
    return fsync(descriptor) == 0 ? CUP_OK : CUP_ERR_FILESYSTEM;
}

static int rename_noreplace_at(int source_parent,
                               const char *source_name,
                               int destination_parent,
                               const char *destination_name) {
#if defined(__linux__) && defined(SYS_renameat2)
#ifndef RENAME_NOREPLACE
#define RENAME_NOREPLACE 1
#endif
    return (int)syscall(SYS_renameat2,
                        source_parent,
                        source_name,
                        destination_parent,
                        destination_name,
                        RENAME_NOREPLACE);
#elif defined(__APPLE__)
    return renameatx_np(source_parent,
                        source_name,
                        destination_parent,
                        destination_name,
                        RENAME_EXCL);
#else
    (void)source_parent;
    (void)source_name;
    (void)destination_parent;
    (void)destination_name;
    errno = ENOTSUP;
    return -1;
#endif
}

/* Process identity, HOME validation and detached uninstall execution. */
void system_set_restrictive_umask(void) {
    umask(0077);
}

static int home_path_is_clean_absolute(const char *path) {
    const char *component;
    const char *cursor;
    size_t length;

    if (text_is_empty(path) || path[0] != '/' || strcmp(path, "/") == 0 ||
        strchr(path, '\\') != NULL || strchr(path, '\n') != NULL || strchr(path, '\r') != NULL) {
        return 0;
    }
    length = strlen(path);
    if (path[length - 1] == '/' || strstr(path, "//") != NULL) {
        return 0;
    }

    cursor = path + 1;
    while (*cursor != '\0') {
        component = cursor;
        while (*cursor != '\0' && *cursor != '/') {
            cursor++;
        }
        length = (size_t)(cursor - component);
        if ((length == 1 && component[0] == '.') ||
            (length == 2 && component[0] == '.' && component[1] == '.')) {
            return 0;
        }
        if (*cursor == '/') {
            cursor++;
        }
    }
    return 1;
}

CupError system_get_home_dir(char *buffer, size_t size) {
    const char *home;

    if (buffer == NULL || size == 0) {
        return CUP_ERR_INVALID_INPUT;
    }

    home = getenv("HOME");
    if (home == NULL || home[0] == '\0') {
        fprintf(stderr, "Error: HOME environment variable is not set.\n");
        return CUP_ERR_FILESYSTEM;
    }

    if (strcmp(home, "/") == 0) {
        fprintf(stderr, "Error: HOME must not be the filesystem root.\n");
        return CUP_ERR_FILESYSTEM;
    }
    if (!home_path_is_clean_absolute(home)) {
        fprintf(stderr, "Error: HOME must contain a clean absolute user directory path.\n");
        return CUP_ERR_FILESYSTEM;
    }

    return text_copy(buffer, size, home);
}

unsigned long system_get_process_id(void) {
    return (unsigned long)getpid();
}

/* A successful handoff consumes the caller-visible lock, but the parent keeps both its lifetime
 * signal and one reference to the shared flock authority until process exit. The child owns a
 * second reference, so either process may die without opening an authority gap before the parent
 * exits. */
static int handoff_parent_signal = -1;
static int handoff_parent_authority = -1;

static CupError start_handoff_helper(const char *helper,
                                     const char *mode,
                                     const char *root,
                                     const char *detached_root,
                                     const char *token,
                                     SystemLock *lock) {
    int parent_fds[2] = {-1, -1};
    int status_fds[2] = {-1, -1};
    int authority_fd = -1;
    pid_t pid;
    char status_byte;
    ssize_t status_count;

    if (text_is_empty(helper) || text_is_empty(mode) || text_is_empty(root) ||
        text_is_empty(token) || lock == NULL || !lock->active ||
        lock->mode != SYSTEM_LOCK_EXCLUSIVE || handoff_parent_signal >= 0 ||
        handoff_parent_authority >= 0) {
        return CUP_ERR_INVALID_INPUT;
    }
    if (pipe(parent_fds) != 0) {
        return CUP_ERR_FILESYSTEM;
    }
    {
        size_t index;

        for (index = 0; index < 2; ++index) {
            if (parent_fds[index] <= STDERR_FILENO) {
                int inherited = fcntl(parent_fds[index], F_DUPFD, STDERR_FILENO + 1);

                if (inherited < 0) {
                    close(parent_fds[0]);
                    close(parent_fds[1]);
                    return CUP_ERR_FILESYSTEM;
                }
                close(parent_fds[index]);
                parent_fds[index] = inherited;
            }
        }
    }
    authority_fd = fcntl((int)lock->handle, F_DUPFD, STDERR_FILENO + 1);
    if (authority_fd < 0) {
        close(parent_fds[0]);
        close(parent_fds[1]);
        return CUP_ERR_FILESYSTEM;
    }
    /* Keep both lifetime endpoints outside the standard descriptor range. Only this cup process
     * owns the write end; prevent any later exec from extending the helper's
     * parent-lifetime signal.
     * The read end and duplicated lock authority stay inherited intentionally and are passed
     * explicitly to the helper. */
    if (fcntl(parent_fds[1], F_SETFD, FD_CLOEXEC) != 0) {
        close(parent_fds[0]);
        close(parent_fds[1]);
        close(authority_fd);
        return CUP_ERR_FILESYSTEM;
    }
    if (pipe(status_fds) != 0) {
        close(parent_fds[0]);
        close(parent_fds[1]);
        close(authority_fd);
        return CUP_ERR_FILESYSTEM;
    }
    if (fcntl(status_fds[1], F_SETFD, FD_CLOEXEC) < 0) {
        close(parent_fds[0]);
        close(parent_fds[1]);
        close(authority_fd);
        close(status_fds[0]);
        close(status_fds[1]);
        return CUP_ERR_FILESYSTEM;
    }

    pid = fork();
    if (pid < 0) {
        close(parent_fds[0]);
        close(parent_fds[1]);
        close(authority_fd);
        close(status_fds[0]);
        close(status_fds[1]);
        return CUP_ERR_FILESYSTEM;
    }
    if (pid == 0) {
        char parent_signal_value[32];
        char authority_value[32];

        close(parent_fds[1]);
        close(status_fds[0]);
        if ((int)lock->handle != authority_fd) {
            close((int)lock->handle);
        }
        if (setsid() < 0 ||
            text_format(parent_signal_value, sizeof(parent_signal_value), "%d", parent_fds[0]) != CUP_OK ||
            text_format(authority_value, sizeof(authority_value), "%d", authority_fd) != CUP_OK) {
            status_byte = 1;
            do {
                status_count = write(status_fds[1], &status_byte, 1);
            } while (status_count < 0 && errno == EINTR);
            _exit(127);
        }
        if (detached_root == NULL) {
            execl(helper,
                  helper,
                  mode,
                  root,
                  token,
                  parent_signal_value,
                  authority_value,
                  (char *)NULL);
        } else {
            execl(helper,
                  helper,
                  mode,
                  root,
                  detached_root,
                  token,
                  parent_signal_value,
                  authority_value,
                  (char *)NULL);
        }
        status_byte = 1;
        do {
            status_count = write(status_fds[1], &status_byte, 1);
        } while (status_count < 0 && errno == EINTR);
        _exit(127);
    }

    close(parent_fds[0]);
    close(authority_fd);
    close(status_fds[1]);
    do {
        status_count = read(status_fds[0], &status_byte, 1);
    } while (status_count < 0 && errno == EINTR);
    close(status_fds[0]);
    if (status_count != 0) {
        pid_t wait_result;

        close(parent_fds[1]);
        do {
            wait_result = waitpid(pid, NULL, 0);
        } while (wait_result < 0 && errno == EINTR);
        return CUP_ERR_FILESYSTEM;
    }

    /* The child owns a duplicate of the same flock open-file description. Consume the public
     * SystemLock without closing or unlocking its descriptor: the parent retains that reference
     * until exit, while the child retains the duplicated authority independently. */
    handoff_parent_authority = (int)lock->handle;
    lock->handle = -1;
    lock->mode = SYSTEM_LOCK_SHARED;
    lock->active = 0;
    handoff_parent_signal = parent_fds[1];
    return CUP_OK;
}

CupError system_start_update_helper(const char *helper,
                                    const char *root,
                                    const char *token,
                                    SystemLock *lock) {
    return start_handoff_helper(
        helper, "--internal-update-helper", root, NULL, token, lock);
}

CupError system_start_uninstall_helper(const char *helper,
                                       const char *root,
                                       const char *detached_root,
                                       const char *token,
                                       SystemLock *lock) {
    if (text_is_empty(detached_root)) {
        return CUP_ERR_INVALID_INPUT;
    }
    return start_handoff_helper(
        helper, "--internal-uninstall-helper", root, detached_root, token, lock);
}

static CupError wait_for_parent_exit(const char *parent_signal_value) {
    unsigned parsed;
    int descriptor;
    char byte;
    ssize_t count;

    if (!text_parse_uint(parent_signal_value, 0x7fffffffu, &parsed) || parsed <= STDERR_FILENO) {
        return CUP_ERR_INVALID_INPUT;
    }

    descriptor = (int)parsed;
    do {
        count = read(descriptor, &byte, 1);
    } while (count > 0 || (count < 0 && errno == EINTR));
    (void)close(descriptor);
    return count < 0 ? CUP_ERR_FILESYSTEM : CUP_OK;
}

CupError system_handoff_accept(SystemHandoff *handoff,
                               const char *parent_signal_value,
                               const char *authority_value) {
    unsigned parsed;
    struct stat info;
    CupError err;

    if (handoff == NULL || handoff->active ||
        !text_parse_uint(authority_value, 0x7fffffffu, &parsed) || parsed <= STDERR_FILENO) {
        return CUP_ERR_INVALID_INPUT;
    }
    err = wait_for_parent_exit(parent_signal_value);
    if (err != CUP_OK) {
        (void)close((int)parsed);
        return err;
    }
    if (fstat((int)parsed, &info) != 0 || !S_ISREG(info.st_mode)) {
        (void)close((int)parsed);
        return CUP_ERR_FILESYSTEM;
    }
    handoff->handle = (intptr_t)(int)parsed;
    handoff->active = 1;
    return CUP_OK;
}

CupError system_handoff_acquire_lock(SystemHandoff *handoff,
                                     SystemLock *lock,
                                     const char *lock_path) {
    if (handoff == NULL || !handoff->active || lock == NULL || lock->active ||
        text_is_empty(lock_path)) {
        return CUP_ERR_INVALID_INPUT;
    }
    lock->handle = handoff->handle;
    lock->mode = SYSTEM_LOCK_EXCLUSIVE;
    lock->active = 1;
    handoff->handle = -1;
    handoff->active = 0;
    return CUP_OK;
}

void system_handoff_release(SystemHandoff *handoff) {
    if (handoff == NULL || !handoff->active) {
        return;
    }
    (void)flock((int)handoff->handle, LOCK_UN);
    (void)close((int)handoff->handle);
    handoff->handle = -1;
    handoff->active = 0;
}

CupError system_get_executable_path(char *buffer, size_t size) {
    char resolved[MAX_PATH_LEN];

    if (buffer == NULL || size == 0) {
        return CUP_ERR_INVALID_INPUT;
    }
#if defined(__linux__)
    {
        ssize_t length = readlink("/proc/self/exe", resolved, sizeof(resolved) - 1);

        if (length <= 0 || (size_t)length >= sizeof(resolved)) {
            return CUP_ERR_FILESYSTEM;
        }
        resolved[length] = '\0';
    }
#elif defined(__APPLE__)
    {
        char raw[MAX_PATH_LEN];
        uint32_t capacity = (uint32_t)sizeof(raw);

        if (_NSGetExecutablePath(raw, &capacity) != 0 || realpath(raw, resolved) == NULL) {
            return CUP_ERR_FILESYSTEM;
        }
    }
#else
#error "Unsupported POSIX platform"
#endif
    if (path_normalize(resolved) != CUP_OK) {
        return CUP_ERR_FILESYSTEM;
    }
    return text_copy(buffer, size, resolved);
}

CupError system_unlink_running_executable(const char *path) {
    char running[MAX_PATH_LEN];
    SystemPathIdentity expected;
    SystemPathIdentity current;
    CupError err;

    if (text_is_empty(path)) {
        return CUP_ERR_INVALID_INPUT;
    }
    err = system_get_executable_path(running, sizeof(running));
    if (err == CUP_OK) {
        err = system_get_path_identity(running, &expected);
    }
    if (err == CUP_OK) {
        err = system_get_path_identity(path, &current);
    }
    if (err != CUP_OK || !expected.valid || expected.kind != SYSTEM_PATH_REGULAR_FILE ||
        !system_path_identity_equal(&expected, &current)) {
        return err != CUP_OK ? err : CUP_ERR_TRANSACTION;
    }
    return system_remove_file_if_identity(path, &current);
}

/* No-follow creation, copy, replacement and recursive mutation primitives. */
CupError system_make_directory(const char *path) {
    char entry[MAX_PATH_LEN];
    int parent_fd = -1;
    struct stat info;
    CupError err;

    if (text_is_empty(path)) {
        return CUP_ERR_INVALID_INPUT;
    }
    err = open_parent_no_follow(path, &parent_fd, entry, sizeof(entry));
    if (err != CUP_OK) {
        return err;
    }

    if (mkdirat(parent_fd, entry, 0755) != 0) {
        int operation_error = errno;

        if (operation_error == EEXIST) {
            if (fstatat(parent_fd, entry, &info, AT_SYMLINK_NOFOLLOW) == 0) {
                if (S_ISDIR(info.st_mode)) {
                    (void)close(parent_fd);
                    return CUP_OK;
                }
                operation_error = EEXIST;
            } else {
                operation_error = errno;
            }
        }
        close(parent_fd);
        fprintf(stderr,
                "Error: could not create directory '%s': %s.\n",
                path,
                strerror(operation_error));
        return CUP_ERR_FILESYSTEM;
    }
    (void)close(parent_fd);
    return CUP_OK;
}

CupError system_check_directory_chain(const char *path, int allow_missing) {
    int descriptor = -1;
    int missing = 0;
    CupError err;

    if (allow_missing != 0 && allow_missing != 1) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = open_directory_path_no_follow_status(path, &descriptor, &missing);
    if (descriptor >= 0) {
        (void)close(descriptor);
    }
    if (err != CUP_OK) {
        return err;
    }
    return missing && !allow_missing ? CUP_ERR_FILESYSTEM : CUP_OK;
}

CupError system_make_directory_chain(const char *path) {
    int descriptor = -1;
    CupError err;

    err = open_directory_path_no_follow_options(path, 1, &descriptor, NULL);
    if (descriptor >= 0) {
        (void)close(descriptor);
    }
    return err;
}

CupError system_directory_is_private(const char *path, int *is_private) {
    int descriptor = -1;
    int missing = 0;
    struct stat info;
    SystemPathKind kind;
    CupError err;

    if (text_is_empty(path) || is_private == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    *is_private = 0;

    err = system_get_path_kind(path, &kind);
    if (err != CUP_OK || kind == SYSTEM_PATH_MISSING || kind != SYSTEM_PATH_DIRECTORY) {
        return err;
    }
    err = open_directory_path_no_follow_status(path, &descriptor, &missing);
    if (err != CUP_OK || missing || descriptor < 0) {
        return err == CUP_OK ? CUP_ERR_FILESYSTEM : err;
    }
    if (fstat(descriptor, &info) != 0) {
        close(descriptor);
        return CUP_ERR_FILESYSTEM;
    }
    if (info.st_uid == geteuid()) {
        *is_private = (info.st_mode & (S_IRWXG | S_IRWXO)) == 0;
    }
    (void)close(descriptor);
    return CUP_OK;
}

CupError system_create_directory_exclusive(const char *path,
                                           unsigned int mode,
                                           SystemCommitState *commit_state) {
    char entry[MAX_PATH_LEN];
    int parent_fd = -1;
    int directory_fd = -1;
    struct stat info;
    CupError err;

    if (text_is_empty(path) || commit_state == NULL || mode > 0777u) {
        return CUP_ERR_INVALID_INPUT;
    }

    *commit_state = SYSTEM_COMMIT_NOT_APPLIED;
    err = open_parent_no_follow(path, &parent_fd, entry, sizeof(entry));
    if (err != CUP_OK) {
        return err;
    }
    if (mkdirat(parent_fd, entry, (mode_t)mode) != 0) {
        int create_error = errno;

        close(parent_fd);
        return create_error == EEXIST ? CUP_ERR_LOCK : CUP_ERR_FILESYSTEM;
    }

    *commit_state = SYSTEM_COMMIT_APPLIED;
    directory_fd = openat(parent_fd,
                          entry,
                          O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (directory_fd < 0 || fstat(directory_fd, &info) != 0 ||
        !S_ISDIR(info.st_mode) || info.st_uid != geteuid() ||
        fchmod(directory_fd, (mode_t)mode) != 0 ||
        fstat(directory_fd, &info) != 0 ||
        (info.st_mode & 0777) != (mode_t)mode || fsync(directory_fd) != 0 ||
        sync_directory_fd(parent_fd) != CUP_OK) {
        if (directory_fd >= 0) {
            close(directory_fd);
        }
        close(parent_fd);
        return CUP_ERR_COMMIT;
    }
    *commit_state = SYSTEM_COMMIT_DURABLE;
    (void)close(directory_fd);
    (void)close(parent_fd);
    return CUP_OK;
}

CupError system_create_private_directory(const char *path,
                                         SystemCommitState *commit_state) {
    CupError err = system_create_directory_exclusive(path, 0700u, commit_state);

    return err == CUP_ERR_LOCK ? CUP_ERR_FILESYSTEM : err;
}

CupError system_make_private_directory(const char *path) {
    int descriptor = -1;
    int missing = 0;
    struct stat info;
    CupError err;

    if (text_is_empty(path)) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = open_directory_path_no_follow_status(path, &descriptor, &missing);
    if (err != CUP_OK) {
        fprintf(stderr, "Error: private directory '%s' is unsafe.\n", path);
        return err;
    }
    if (missing) {
        SystemCommitState commit_state = SYSTEM_COMMIT_NOT_APPLIED;

        err = system_create_private_directory(path, &commit_state);
        if (err == CUP_OK) {
            return CUP_OK;
        }
        if (commit_state != SYSTEM_COMMIT_NOT_APPLIED) {
            return err;
        }

        /* A concurrent creator may have won after the missing check. Re-open the
         * final directory through the same no-follow chain before accepting it. */
        err = open_directory_path_no_follow_status(path, &descriptor, &missing);
        if (err != CUP_OK || missing || descriptor < 0) {
            fprintf(stderr, "Error: could not create private directory '%s'.\n", path);
            return err == CUP_OK ? CUP_ERR_FILESYSTEM : err;
        }
    }

    if (descriptor < 0 || fstat(descriptor, &info) != 0 || !S_ISDIR(info.st_mode) ||
        info.st_uid != geteuid()) {
        if (descriptor >= 0) {
            close(descriptor);
        }
        fprintf(stderr,
                "Error: private directory '%s' is not owned by the current user or is unsafe.\n",
                path);
        return CUP_ERR_FILESYSTEM;
    }
    if ((info.st_mode & 0777) != 0700 && fchmod(descriptor, 0700) != 0) {
        close(descriptor);
        fprintf(stderr, "Error: could not secure directory '%s': %s.\n", path, strerror(errno));
        return CUP_ERR_FILESYSTEM;
    }
    (void)close(descriptor);
    return CUP_OK;
}

CupError system_remove_directory(const char *path) {
    char entry[MAX_PATH_LEN];
    int parent_fd = -1;
    struct stat observed;
    struct stat current;
    CupError err;

    if (text_is_empty(path)) {
        return CUP_ERR_INVALID_INPUT;
    }
    err = open_parent_no_follow(path, &parent_fd, entry, sizeof(entry));
    if (err != CUP_OK) {
        return err;
    }
    if (fstatat(parent_fd, entry, &observed, AT_SYMLINK_NOFOLLOW) != 0) {
        int status_error = errno;

        close(parent_fd);
        return status_error == ENOENT ? CUP_OK : CUP_ERR_FILESYSTEM;
    }
    if (!S_ISDIR(observed.st_mode)) {
        close(parent_fd);
        return CUP_ERR_FILESYSTEM;
    }

    /* rmdir authority comes from the parent directory. Recheck the final entry immediately
     * before deletion instead of requiring read access to an otherwise removable directory. */
    if (fstatat(parent_fd, entry, &current, AT_SYMLINK_NOFOLLOW) != 0 ||
        !stat_identity_equal(&observed, &current)) {
        close(parent_fd);
        return CUP_ERR_FILESYSTEM;
    }
    if (unlinkat(parent_fd, entry, AT_REMOVEDIR) != 0) {
        int remove_errno = errno;

        close(parent_fd);
        errno = remove_errno;
        fprintf(stderr, "Error: could not remove directory '%s': %s.\n", path, strerror(errno));
        return CUP_ERR_FILESYSTEM;
    }
    (void)close(parent_fd);
    return CUP_OK;
}

static CupError move_path_common(const char *source,
                                 const char *destination,
                                 int replace,
                                 const SystemPathIdentity *expected_source,
                                 const SystemPathIdentity *expected_destination,
                                 SystemCommitState *commit_state) {
    char source_name[MAX_PATH_LEN];
    char destination_name[MAX_PATH_LEN];
    int source_parent = -1;
    int destination_parent = -1;
    struct stat source_info;
    struct stat destination_info;
    CupError result = CUP_OK;
    int operation_result;

    if (text_is_empty(source) || text_is_empty(destination) || commit_state == NULL ||
        (expected_source != NULL &&
         (!expected_source->valid ||
          (expected_source->kind != SYSTEM_PATH_REGULAR_FILE &&
           expected_source->kind != SYSTEM_PATH_DIRECTORY))) ||
        (expected_destination != NULL && !expected_destination->valid)) {
        return CUP_ERR_INVALID_INPUT;
    }
    *commit_state = SYSTEM_COMMIT_NOT_APPLIED;

    /* Resolve both parents without following links before observing either endpoint. */
    result = open_parent_no_follow(
        source, &source_parent, source_name, sizeof(source_name));
    if (result == CUP_OK) {
        result = open_parent_no_follow(
            destination, &destination_parent, destination_name, sizeof(destination_name));
    }
    if (result != CUP_OK) {
        goto cleanup;
    }

    /* Observe the source through its pinned parent without requiring data access. Rename
     * authority comes from the parent directory; unreadable managed files must remain movable. */
    if (fstatat(source_parent, source_name, &source_info, AT_SYMLINK_NOFOLLOW) != 0 ||
        (!S_ISREG(source_info.st_mode) && !S_ISDIR(source_info.st_mode))) {
        result = CUP_ERR_FILESYSTEM;
        goto cleanup;
    }
    if (expected_source != NULL) {
        SystemPathIdentity observed_source;

        identity_from_stat(&source_info, &observed_source);
        if (!system_path_identity_equal(&observed_source, expected_source)) {
            result = CUP_ERR_TRANSACTION;
            goto cleanup;
        }
    }

    /* Revalidate the source name immediately before the single rename commit. A parent-relative
     * rename cannot atomically bind to a file descriptor, so the name must still identify exactly
     * the object observed above; post-commit verification closes the remaining narrow race. */
    system_test_pause("before-move-commit");
    {
        struct stat current_source;

        if (fstatat(source_parent, source_name, &current_source, AT_SYMLINK_NOFOLLOW) != 0 ||
            !stat_identity_equal(&source_info, &current_source)) {
            result = CUP_ERR_INCONSISTENT_STATE;
            goto cleanup;
        }
    }

    /* Validate the destination policy immediately before the single rename commit. */
    if (!replace) {
        operation_result = rename_noreplace_at(source_parent,
                                               source_name,
                                               destination_parent,
                                               destination_name);
    } else {
        struct stat existing;
        int destination_result;

        if (!S_ISREG(source_info.st_mode)) {
            result = CUP_ERR_FILESYSTEM;
            goto cleanup;
        }

        errno = 0;
        destination_result = fstatat(destination_parent,
                                     destination_name,
                                     &existing,
                                     AT_SYMLINK_NOFOLLOW);
        if (destination_result == 0 && !S_ISREG(existing.st_mode)) {
            result = CUP_ERR_FILESYSTEM;
            goto cleanup;
        }
        if (destination_result == 0 && expected_destination != NULL) {
            SystemPathIdentity observed_identity;

            identity_from_stat(&existing, &observed_identity);
            if (!system_path_identity_equal(&observed_identity, expected_destination)) {
                result = CUP_ERR_TRANSACTION;
                goto cleanup;
            }
        }
        if (destination_result != 0) {
            if (errno == ENOENT && expected_destination != NULL) {
                result = CUP_ERR_TRANSACTION;
            } else if (errno != ENOENT) {
                result = CUP_ERR_FILESYSTEM;
            }
            if (result != CUP_OK) {
                goto cleanup;
            }
        }

        operation_result = renameat(source_parent,
                                    source_name,
                                    destination_parent,
                                    destination_name);
    }

    if (operation_result != 0) {
        int operation_errno = errno;

        if (!replace && (operation_errno == EEXIST || operation_errno == ENOTEMPTY)) {
            fprintf(stderr, "Error: destination '%s' already exists.\n", destination);
        } else {
            fprintf(stderr,
                    "Error: could not move '%s' to '%s': %s.\n",
                    source,
                    destination,
                    strerror(operation_errno));
        }
        result = CUP_ERR_FILESYSTEM;
        goto cleanup;
    }

    *commit_state = SYSTEM_COMMIT_APPLIED;

    /* Prove that the committed destination is the source object. A no-replace move can be
     * rolled back when that proof fails; replacement cannot recover the previous destination. */
    if (fstatat(destination_parent,
                destination_name,
                &destination_info,
                AT_SYMLINK_NOFOLLOW) != 0 ||
        !stat_identity_equal(&source_info, &destination_info)) {
        if (!replace &&
            rename_noreplace_at(destination_parent,
                                destination_name,
                                source_parent,
                                source_name) == 0) {
            CupError rollback_sync = sync_directory_fd(source_parent);

            if (source_parent != destination_parent &&
                sync_directory_fd(destination_parent) != CUP_OK) {
                rollback_sync = CUP_ERR_FILESYSTEM;
            }
            *commit_state = rollback_sync == CUP_OK ? SYSTEM_COMMIT_NOT_APPLIED
                                                    : SYSTEM_COMMIT_APPLIED;
            result = rollback_sync == CUP_OK ? CUP_ERR_INCONSISTENT_STATE
                                             : CUP_ERR_COMMIT;
        } else {
            result = CUP_ERR_COMMIT;
        }
        goto cleanup;
    }

    /* The rename is visible at this point. Parent synchronization decides whether it is durable. */
    result = sync_directory_fd(destination_parent);
    if (source_parent != destination_parent &&
        sync_directory_fd(source_parent) != CUP_OK) {
        result = CUP_ERR_FILESYSTEM;
    }
    if (result == CUP_OK) {
        *commit_state = SYSTEM_COMMIT_DURABLE;
    }

cleanup:
    if (source_parent >= 0) {
        (void)close(source_parent);
    }
    if (destination_parent >= 0 && destination_parent != source_parent) {
        (void)close(destination_parent);
    }
    return result;
}

CupError system_move_path(const char *source,
                          const char *destination,
                          SystemCommitState *commit_state) {
    return move_path_common(source, destination, 0, NULL, NULL, commit_state);
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
    return move_path_common(source, destination, 0, expected_identity, NULL, commit_state);
}

CupError system_move_path_retry(const char *source,
                                const char *destination,
                                const SystemPathIdentity *expected_identity,
                                SystemCommitState *commit_state) {
    return system_move_path_if_identity(source, destination, expected_identity, commit_state);
}

CupError system_replace_file(const char *source,
                             const char *destination,
                             SystemCommitState *commit_state) {
    return move_path_common(source, destination, 1, NULL, NULL, commit_state);
}

CupError system_replace_file_if_identity(const char *source,
                                         const char *destination,
                                         const SystemPathIdentity *expected_identity,
                                         SystemCommitState *commit_state) {
    if (expected_identity == NULL || !expected_identity->valid ||
        expected_identity->kind != SYSTEM_PATH_REGULAR_FILE) {
        return CUP_ERR_INVALID_INPUT;
    }
    return move_path_common(source, destination, 1, NULL, expected_identity, commit_state);
}

static CupError split_parent_entry(
    const char *path, char *parent, size_t parent_size, char *entry, size_t entry_size) {
    char copy[MAX_PATH_LEN];
    char *slash;
    size_t length;

    if (text_is_empty(path) || parent == NULL || parent_size == 0 || entry == NULL ||
        entry_size == 0) {
        return CUP_ERR_INVALID_INPUT;
    }
    if (text_copy(copy, sizeof(copy), path) != CUP_OK) {
        return CUP_ERR_BUFFER_TOO_SMALL;
    }

    length = strlen(copy);
    while (length > 1 && copy[length - 1] == '/') {
        copy[--length] = '\0';
    }
    slash = strrchr(copy, '/');
    if (slash == NULL) {
        if (text_copy(parent, parent_size, ".") != CUP_OK ||
            text_copy(entry, entry_size, copy) != CUP_OK) {
            return CUP_ERR_BUFFER_TOO_SMALL;
        }
    } else {
        if (slash == copy) {
            if (text_copy(entry, entry_size, slash + 1) != CUP_OK) {
                return CUP_ERR_BUFFER_TOO_SMALL;
            }
            slash[1] = '\0';
            if (text_copy(parent, parent_size, copy) != CUP_OK) {
                return CUP_ERR_BUFFER_TOO_SMALL;
            }
        } else {
            *slash = '\0';
            if (text_copy(parent, parent_size, copy) != CUP_OK ||
                text_copy(entry, entry_size, slash + 1) != CUP_OK) {
                return CUP_ERR_BUFFER_TOO_SMALL;
            }
        }
    }

    if (text_is_empty(entry) || strcmp(entry, ".") == 0 || strcmp(entry, "..") == 0) {
        return CUP_ERR_INVALID_INPUT;
    }
    return CUP_OK;
}

#define SYSTEM_MAX_TREE_DEPTH 128u

static CupError remove_entry_at(int parent_fd,
                                const char *name,
                                dev_t root_device,
                                unsigned int depth,
                                int (*cancelled)(void)) {
    struct stat observed;
    struct stat current;

    if (parent_fd < 0 || text_is_empty(name) || strcmp(name, ".") == 0 ||
        strcmp(name, "..") == 0) {
        return CUP_ERR_INVALID_INPUT;
    }
    if (depth > SYSTEM_MAX_TREE_DEPTH) {
        return CUP_ERR_FILESYSTEM;
    }
    if (cancelled != NULL && cancelled()) {
        return CUP_ERR_INTERRUPT;
    }
    if (fstatat(parent_fd, name, &observed, AT_SYMLINK_NOFOLLOW) != 0) {
        return errno == ENOENT ? CUP_OK : CUP_ERR_FILESYSTEM;
    }
    if (system_test_crosses_boundary(name, observed.st_dev, root_device)) {
        fprintf(stderr, "Error: refusing to cross a filesystem boundary at '%s'.\n", name);
        return CUP_ERR_FILESYSTEM;
    }

    if (S_ISDIR(observed.st_mode)) {
        int child_fd;
        DIR *directory;
        struct dirent *child;
        CupError err = CUP_OK;

        child_fd = openat(parent_fd,
                          name,
                          O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (child_fd < 0 || fstat(child_fd, &current) != 0 ||
            !stat_identity_equal(&observed, &current)) {
            if (child_fd >= 0) {
                close(child_fd);
            }
            return CUP_ERR_FILESYSTEM;
        }
        directory = fdopendir(child_fd);
        if (directory == NULL) {
            close(child_fd);
            return CUP_ERR_FILESYSTEM;
        }
        child_fd = dirfd(directory);
        if (child_fd < 0) {
            closedir(directory);
            return CUP_ERR_FILESYSTEM;
        }

        errno = 0;
        while ((child = readdir(directory)) != NULL) {
            if (strcmp(child->d_name, ".") == 0 || strcmp(child->d_name, "..") == 0) {
                continue;
            }
            err = remove_entry_at(
                child_fd, child->d_name, root_device, depth + 1u, cancelled);
            if (err != CUP_OK) {
                break;
            }
            errno = 0;
        }
        if (err == CUP_OK && errno != 0) {
            err = CUP_ERR_FILESYSTEM;
        }
        if (fstat(child_fd, &current) != 0 ||
            !stat_identity_equal(&observed, &current)) {
            err = CUP_ERR_FILESYSTEM;
        }
        (void)closedir(directory);
        if (err != CUP_OK) {
            return err;
        }
        if (fstatat(parent_fd, name, &current, AT_SYMLINK_NOFOLLOW) != 0 ||
            !stat_identity_equal(&observed, &current)) {
            return CUP_ERR_FILESYSTEM;
        }
        /* Recursive deletion is intentionally interruptible between destructive steps.
         * Re-check after the children have been removed and immediately before removing
         * the now-empty directory, rather than treating the entry-start check as sufficient. */
        if (cancelled != NULL && cancelled()) {
            return CUP_ERR_INTERRUPT;
        }
        if (unlinkat(parent_fd, name, AT_REMOVEDIR) != 0 && errno != ENOENT) {
            return CUP_ERR_FILESYSTEM;
        }
        return CUP_OK;
    }

    /* Revalidate the final name immediately before unlink. Deletion is authorized by the
     * parent directory, not by read access to the entry, so do not require an O_RDONLY
     * descriptor that would make an otherwise removable mode-000 file undeletable. */
    if (fstatat(parent_fd, name, &current, AT_SYMLINK_NOFOLLOW) != 0 ||
        !stat_identity_equal(&observed, &current)) {
        return CUP_ERR_FILESYSTEM;
    }
    if (cancelled != NULL && cancelled()) {
        return CUP_ERR_INTERRUPT;
    }
    if (unlinkat(parent_fd, name, 0) != 0 && errno != ENOENT) {
        return CUP_ERR_FILESYSTEM;
    }
    return CUP_OK;
}

static CupError remove_file_common(const char *path,
                                   const SystemPathIdentity *expected_identity) {
    char entry[MAX_PATH_LEN];
    int parent_fd = -1;
    struct stat observed;
    struct stat current;
    SystemPathIdentity observed_identity;
    CupError err;

    if (text_is_empty(path) ||
        (expected_identity != NULL && !expected_identity->valid)) {
        return CUP_ERR_INVALID_INPUT;
    }
    err = open_parent_no_follow(path, &parent_fd, entry, sizeof(entry));
    if (err != CUP_OK) {
        return err;
    }
    if (fstatat(parent_fd, entry, &observed, AT_SYMLINK_NOFOLLOW) != 0) {
        int status_error = errno;

        close(parent_fd);
        return status_error == ENOENT && expected_identity == NULL ? CUP_OK
                                                                   : CUP_ERR_FILESYSTEM;
    }
    if (S_ISDIR(observed.st_mode)) {
        close(parent_fd);
        return CUP_ERR_FILESYSTEM;
    }
    identity_from_stat(&observed, &observed_identity);
    if (expected_identity != NULL &&
        !system_path_identity_equal(&observed_identity, expected_identity)) {
        close(parent_fd);
        return CUP_ERR_TRANSACTION;
    }
    system_test_pause("before-remove-file-component");
    /* The parent descriptor owns unlink authority. Revalidate the exact final entry immediately
     * before deletion without opening it for data access, so unreadable regular files remain
     * removable while symlinks and other non-directory entries keep their no-follow identity. */
    if (fstatat(parent_fd, entry, &current, AT_SYMLINK_NOFOLLOW) != 0 ||
        !stat_identity_equal(&observed, &current)) {
        close(parent_fd);
        return CUP_ERR_FILESYSTEM;
    }
    if (unlinkat(parent_fd, entry, 0) != 0) {
        close(parent_fd);
        return CUP_ERR_FILESYSTEM;
    }
    /* The unlink is the mutation boundary. Closing this read-only parent descriptor cannot undo
     * it. This API has no commit-state channel for an already-applied result. */
    (void)close(parent_fd);
    return CUP_OK;
}

CupError system_remove_file(const char *path) {
    return remove_file_common(path, NULL);
}

CupError system_remove_file_if_identity(const char *path,
                                        const SystemPathIdentity *expected_identity) {
    if (expected_identity == NULL || !expected_identity->valid ||
        expected_identity->kind != SYSTEM_PATH_REGULAR_FILE) {
        return CUP_ERR_INVALID_INPUT;
    }
    return remove_file_common(path, expected_identity);
}

CupError system_remove_tree_contents(const char *path,
                                     const char *preserve_name,
                                     int (*cancelled)(void)) {
    int descriptor = -1;
    int scan = -1;
    DIR *directory = NULL;
    struct dirent *entry;
    struct stat root_info;
    CupError err = CUP_OK;

    if (text_is_empty(path) ||
        (preserve_name != NULL && !path_is_safe_segment(preserve_name))) {
        return CUP_ERR_INVALID_INPUT;
    }
    if (cancelled != NULL && cancelled()) {
        return CUP_ERR_INTERRUPT;
    }
    err = open_directory_path_no_follow(path, &descriptor);
    if (err != CUP_OK) {
        return err;
    }
    if (fstat(descriptor, &root_info) != 0) {
        close(descriptor);
        return CUP_ERR_FILESYSTEM;
    }

    scan = dup(descriptor);
    if (scan < 0 || fcntl(scan, F_SETFD, FD_CLOEXEC) != 0 ||
        (directory = fdopendir(scan)) == NULL) {
        if (scan >= 0) {
            close(scan);
        }
        close(descriptor);
        return CUP_ERR_FILESYSTEM;
    }

    errno = 0;
    while ((entry = readdir(directory)) != NULL) {
        char name[MAX_PATH_LEN];

        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0 ||
            (preserve_name != NULL && strcmp(entry->d_name, preserve_name) == 0)) {
            continue;
        }
        if (text_copy(name, sizeof(name), entry->d_name) != CUP_OK) {
            err = CUP_ERR_BUFFER_TOO_SMALL;
            break;
        }

        err = remove_entry_at(descriptor, name, root_info.st_dev, 0u, cancelled);
        if (err != CUP_OK) {
            break;
        }
        errno = 0;
    }
    if (err == CUP_OK && errno != 0) {
        err = CUP_ERR_FILESYSTEM;
    }
    /* Child removals are already applied before stream/descriptor cleanup. Only the
     * directory fsync can still change the operation result by proving durability. */
    (void)closedir(directory);
    if (err == CUP_OK && sync_directory_fd(descriptor) != CUP_OK) {
        err = CUP_ERR_FILESYSTEM;
    }
    (void)close(descriptor);
    return err;
}

static CupError remove_tree_common(const char *path,
                                   const SystemPathIdentity *expected_identity,
                                   int (*cancelled)(void)) {
    char entry[MAX_PATH_LEN];
    int parent_fd = -1;
    struct stat root_info;
    SystemPathIdentity observed_identity;
    CupError err;

    if (text_is_empty(path) ||
        (expected_identity != NULL && !expected_identity->valid)) {
        return CUP_ERR_INVALID_INPUT;
    }
    if (cancelled != NULL && cancelled()) {
        return CUP_ERR_INTERRUPT;
    }

    err = open_parent_no_follow(path, &parent_fd, entry, sizeof(entry));
    if (err != CUP_OK) {
        return err;
    }

    system_test_pause("before-remove-target");
    if (fstatat(parent_fd, entry, &root_info, AT_SYMLINK_NOFOLLOW) != 0) {
        int status_error = errno;

        close(parent_fd);
        return status_error == ENOENT && expected_identity == NULL ? CUP_OK
                                                                   : CUP_ERR_FILESYSTEM;
    }

    identity_from_stat(&root_info, &observed_identity);
    if (expected_identity != NULL &&
        !system_path_identity_equal(&observed_identity, expected_identity)) {
        close(parent_fd);
        return CUP_ERR_TRANSACTION;
    }

    err = remove_entry_at(parent_fd, entry, root_info.st_dev, 0u, cancelled);
    /* A successful recursive deletion is already applied. Parent-descriptor cleanup must not
     * turn that result into an indistinguishable pre-commit filesystem failure. */
    (void)close(parent_fd);
    return err;
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
    return remove_file_common(path, expected_identity);
}

CupError system_remove_tree(const char *path, int (*cancelled)(void)) {
    return remove_tree_common(path, NULL, cancelled);
}

CupError system_copy_file(const char *source_path, const char *destination_path) {
    SystemCommitState commit_state = SYSTEM_COMMIT_NOT_APPLIED;
    FILE *source = NULL;
    FILE *destination = NULL;
    unsigned char buffer[8192];
    size_t count;
    int failed = 0;
    int missing = 0;
    uint64_t source_size = 0;
    SystemPathIdentity source_identity;
    struct stat source_info;
    CupError err;
    char parent[MAX_PATH_LEN];
    char temporary[MAX_PATH_LEN] = "";

    if (text_is_empty(source_path) || text_is_empty(destination_path)) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = system_open_regular_file(
        source_path, &source, &source_identity, &source_size, &missing);
    if (err != CUP_OK || missing || source == NULL || !source_identity.valid ||
        fstat(fileno(source), &source_info) != 0) {
        if (source != NULL) {
            fclose(source);
        }
        return err != CUP_OK ? err : CUP_ERR_FILESYSTEM;
    }

    err = path_parent(parent, sizeof(parent), destination_path);
    if (err == CUP_OK) {
        err = system_create_temp_file(
            parent, "copy", temporary, sizeof(temporary), &destination);
    }
    if (err != CUP_OK) {
        fclose(source);
        /* The sibling temporary file is an implementation detail of copying. Preserve
         * caller-capacity errors, but expose an OS failure to create it as a filesystem error. */
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

    if (!failed && fchmod(fileno(destination), source_info.st_mode & 0777) != 0) {
        failed = 1;
    }
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
    struct stat info;
    mode_t mode;
    int descriptor;

    if (file == NULL || (executable != 0 && executable != 1)) {
        return CUP_ERR_INVALID_INPUT;
    }
    descriptor = fileno(file);
    if (descriptor < 0 || fstat(descriptor, &info) != 0 || !S_ISREG(info.st_mode)) {
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
        mode &= ~(mode_t)(S_IXUSR | S_IXGRP | S_IXOTH);
    }
    return fchmod(descriptor, mode) == 0 ? CUP_OK : CUP_ERR_FILESYSTEM;
}

CupError system_file_is_executable(FILE *file, const char *path, int *is_executable) {
    struct stat info;
    int descriptor;

    if (file == NULL || text_is_empty(path) || is_executable == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    *is_executable = 0;
    descriptor = fileno(file);
    if (descriptor < 0 || fstat(descriptor, &info) != 0 || !S_ISREG(info.st_mode)) {
        return CUP_ERR_FILESYSTEM;
    }
    *is_executable = (info.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH)) != 0;
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
    CupError err = path_parent(parent, sizeof(parent), path);

    return err == CUP_OK ? sync_directory(parent) : err;
}

/* Unpredictable create-exclusive temporary files and directories. */
static CupError validate_temp_directory(const char *directory) {
    int descriptor = -1;
    int missing = 0;
    CupError err;

    err = open_directory_path_no_follow_status(directory, &descriptor, &missing);
    if (err != CUP_OK) {
        return err;
    }
    if (missing || descriptor < 0) {
        return CUP_ERR_TEMPORARY;
    }
    (void)close(descriptor);
    return CUP_OK;
}

CupError system_create_file_exclusive(const char *path, FILE **file) {
    char entry[MAX_PATH_LEN];
    int parent_fd = -1;
    int fd;
    CupError err;

    if (text_is_empty(path) || file == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    *file = NULL;

    err = open_parent_no_follow(path, &parent_fd, entry, sizeof(entry));
    if (err != CUP_OK) {
        return err;
    }
    fd = openat(parent_fd, entry, O_CREAT | O_EXCL | O_RDWR | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) {
        int open_error = errno;

        close(parent_fd);
        return open_error == EEXIST ? CUP_ERR_LOCK : CUP_ERR_FILESYSTEM;
    }

    *file = fdopen(fd, "w+b");
    if (*file == NULL) {
        int stream_error = errno;

        close(fd);
        (void)unlinkat(parent_fd, entry, 0);
        close(parent_fd);
        errno = stream_error;
        return CUP_ERR_FILESYSTEM;
    }
    (void)close(parent_fd);
    return CUP_OK;
}

CupError system_create_temp_file(
    const char *directory, const char *prefix, char *path, size_t path_size, FILE **file) {
    int fd;
    CupError err;

    if (text_is_empty(directory) || !path_is_safe_segment(prefix) || path == NULL ||
        path_size == 0 || file == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    *file = NULL;
    err = text_format(path, path_size, "%s/%s-XXXXXX", directory, prefix);
    if (err != CUP_OK) {
        return err;
    }
    err = validate_temp_directory(directory);
    if (err != CUP_OK) {
        return err;
    }
    fd = mkstemp(path);
    if (fd < 0) {
        return CUP_ERR_TEMPORARY;
    }

    if (fcntl(fd, F_SETFD, FD_CLOEXEC) != 0 || fchmod(fd, 0600) != 0) {
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

CupError system_create_temp_directory(const char *directory,
                                      const char *prefix,
                                      char *path,
                                      size_t path_size) {
    CupError err;

    if (text_is_empty(directory) || !path_is_safe_segment(prefix) || path == NULL ||
        path_size == 0) {
        return CUP_ERR_INVALID_INPUT;
    }
    err = text_format(path, path_size, "%s/%s-XXXXXX", directory, prefix);
    if (err != CUP_OK) {
        return err;
    }
    err = validate_temp_directory(directory);
    if (err != CUP_OK) {
        return err;
    }

    /* mkdtemp creates the directory with owner-only mode 0700. Do not apply a
     * second pathname-based chmod after creation: the created name is already
     * the exclusive object returned by the primitive. */
    return mkdtemp(path) != NULL ? CUP_OK : CUP_ERR_TEMPORARY;
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

    (void)fclose(file);
    return unlink(path) == 0 ? CUP_OK : CUP_ERR_TEMPORARY;
}

/* Path inspection and permissions without link following. */
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
        fprintf(stderr, "Error: could not inspect path '%s': %s.\n", path, strerror(errno));
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


CupError system_open_regular_file(const char *path,
                                  FILE **file,
                                  SystemPathIdentity *identity,
                                  uint64_t *file_size,
                                  int *missing) {
    char entry[MAX_PATH_LEN];
    struct stat info;
    int parent_fd = -1;
    int fd = -1;
    CupError err;

    if (text_is_empty(path) || file == NULL || identity == NULL || file_size == NULL ||
        missing == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    *file = NULL;
    *file_size = 0;
    *missing = 0;
    memset(identity, 0, sizeof(*identity));

    err = open_parent_no_follow_status(
        path, &parent_fd, entry, sizeof(entry), missing);
    if (err != CUP_OK) {
        *missing = 0;
        return err;
    }
    if (*missing) {
        return CUP_OK;
    }
    fd = openat(parent_fd, entry, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
        int open_error = errno;
        close(parent_fd);
        if (open_error == ENOENT || open_error == ENOTDIR) {
            *missing = 1;
            return CUP_OK;
        }
        return CUP_ERR_FILESYSTEM;
    }
    close(parent_fd);
    if (fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) || info.st_size < 0) {
        close(fd);
        return CUP_ERR_FILESYSTEM;
    }
    identity_from_stat(&info, identity);
    *file_size = (uint64_t)info.st_size;
    *file = fdopen(fd, "rb");
    if (*file == NULL) {
        close(fd);
        memset(identity, 0, sizeof(*identity));
        *file_size = 0;
        return CUP_ERR_FILESYSTEM;
    }
    return CUP_OK;
}

CupError system_get_path_identity(const char *path, SystemPathIdentity *identity) {
    struct stat info;

    if (identity == NULL || text_is_empty(path)) {
        return CUP_ERR_INVALID_INPUT;
    }
    memset(identity, 0, sizeof(*identity));
    if (lstat(path, &info) != 0) {
        if (errno == ENOENT || errno == ENOTDIR) {
            return CUP_OK;
        }
        return CUP_ERR_FILESYSTEM;
    }
    identity_from_stat(&info, identity);
    return CUP_OK;
}

int system_path_identity_equal(const SystemPathIdentity *left,
                               const SystemPathIdentity *right) {
    return left != NULL && right != NULL && left->valid && right->valid &&
           left->volume == right->volume && left->object == right->object &&
           left->object_high == right->object_high && left->kind == right->kind;
}

CupError system_file_size(const char *path, long long *file_size) {
    struct stat info;

    if (file_size == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    *file_size = 0;
    if (text_is_empty(path)) {
        return CUP_ERR_INVALID_INPUT;
    }
    if (lstat(path, &info) != 0 || !S_ISREG(info.st_mode)) {
        return CUP_ERR_FILESYSTEM;
    }
    *file_size = (long long)info.st_size;
    return CUP_OK;
}

/* Owner-only directory policy plus executable and read-only file controls. */
CupError system_is_executable(const char *path, int *is_executable) {
    struct stat info;

    if (is_executable == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    *is_executable = 0;
    if (text_is_empty(path)) {
        return CUP_ERR_INVALID_INPUT;
    }
    if (lstat(path, &info) != 0) {
        return errno == ENOENT || errno == ENOTDIR ? CUP_OK : CUP_ERR_FILESYSTEM;
    }

    *is_executable = S_ISREG(info.st_mode) &&
                     (info.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH)) != 0;
    return CUP_OK;
}

CupError system_is_read_only(const char *path, int *is_read_only) {
    struct stat info;

    if (is_read_only == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    *is_read_only = 0;
    if (text_is_empty(path)) {
        return CUP_ERR_INVALID_INPUT;
    }
    if (lstat(path, &info) != 0 ||
        (!S_ISREG(info.st_mode) && !S_ISDIR(info.st_mode))) {
        return CUP_ERR_FILESYSTEM;
    }
    *is_read_only = (info.st_mode & (S_IWUSR | S_IWGRP | S_IWOTH)) == 0;
    return CUP_OK;
}

static CupError open_permission_target(const char *path, int regular_only, int *descriptor) {
    char entry[MAX_PATH_LEN];
    int parent_fd = -1;
    int target_fd = -1;
    struct stat info;
    CupError err;

    if (text_is_empty(path) || descriptor == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    *descriptor = -1;

    err = open_parent_no_follow(path, &parent_fd, entry, sizeof(entry));
    if (err != CUP_OK) {
        return err;
    }
    target_fd = openat(parent_fd,
                       entry,
                       O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
    if (target_fd < 0 || fstat(target_fd, &info) != 0 ||
        (regular_only ? !S_ISREG(info.st_mode)
                      : (!S_ISREG(info.st_mode) && !S_ISDIR(info.st_mode)))) {
        if (target_fd >= 0) {
            close(target_fd);
        }
        close(parent_fd);
        return CUP_ERR_FILESYSTEM;
    }
    (void)close(parent_fd);

    *descriptor = target_fd;
    return CUP_OK;
}

CupError system_set_read_only(const char *path, int read_only) {
    struct stat info;
    mode_t mode;
    int descriptor = -1;
    CupError err;

    if (read_only != 0 && read_only != 1) {
        return CUP_ERR_INVALID_INPUT;
    }
    err = open_permission_target(path, 0, &descriptor);
    if (err != CUP_OK) {
        return err;
    }
    if (fstat(descriptor, &info) != 0) {
        close(descriptor);
        return CUP_ERR_FILESYSTEM;
    }

    mode = info.st_mode;
    if (read_only) {
        mode &= ~(mode_t)(S_IWUSR | S_IWGRP | S_IWOTH);
    } else {
        mode |= S_IWUSR;
    }

    {
        int chmod_failed = fchmod(descriptor, mode) != 0;

        (void)close(descriptor);
        return chmod_failed ? CUP_ERR_FILESYSTEM : CUP_OK;
    }
}

CupError system_set_executable(const char *path, int executable) {
    struct stat info;
    mode_t mode;
    int descriptor = -1;
    CupError err;

    if (executable != 0 && executable != 1) {
        return CUP_ERR_INVALID_INPUT;
    }
    err = open_permission_target(path, 1, &descriptor);
    if (err != CUP_OK) {
        return err;
    }
    if (fstat(descriptor, &info) != 0) {
        close(descriptor);
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
        mode &= ~(mode_t)(S_IXUSR | S_IXGRP | S_IXOTH);
    }

    {
        int chmod_failed = fchmod(descriptor, mode) != 0;

        (void)close(descriptor);
        return chmod_failed ? CUP_ERR_FILESYSTEM : CUP_OK;
    }
}

/* Descriptor-anchored child enumeration that never follows a child symlink. */
CupError system_list_directory(const char *path,
                               SystemDirectoryCallback callback,
                               void *userdata) {
    int descriptor = -1;
    int missing = 0;
    DIR *directory;
    struct dirent *entry;
    CupError err = CUP_OK;

    if (callback == NULL || text_is_empty(path)) {
        return CUP_ERR_INVALID_INPUT;
    }
    err = open_directory_path_no_follow_status(path, &descriptor, &missing);
    if (err != CUP_OK || missing) {
        return err;
    }

    directory = fdopendir(descriptor);
    if (directory == NULL) {
        close(descriptor);
        return CUP_ERR_FILESYSTEM;
    }
    descriptor = dirfd(directory);
    if (descriptor < 0) {
        closedir(directory);
        return CUP_ERR_FILESYSTEM;
    }

    errno = 0;
    while ((entry = readdir(directory)) != NULL) {
        struct stat observed;
        char child_path[MAX_PATH_LEN];
        SystemPathKind kind;

        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        if (fstatat(descriptor, entry->d_name, &observed, AT_SYMLINK_NOFOLLOW) != 0) {
            err = CUP_ERR_FILESYSTEM;
            break;
        }
        err = path_join(child_path, sizeof(child_path), path, entry->d_name);
        if (err != CUP_OK) {
            break;
        }

        {
            SystemPathIdentity identity;

            kind = path_kind_from_mode(observed.st_mode);
            identity_from_stat(&observed, &identity);
            err = callback(child_path, kind, &identity, userdata);
        }
        if (err != CUP_OK) {
            break;
        }
        errno = 0;
    }
    if (err == CUP_OK && errno != 0) {
        err = CUP_ERR_FILESYSTEM;
    }
    (void)closedir(directory);
    return err;
}


static CupError walk_directory_fd(int descriptor,
                                  const char *display,
                                  dev_t root_device,
                                  unsigned int depth,
                                  SystemDirectoryCallback callback,
                                  void *userdata) {
    int scan;
    DIR *directory;
    struct dirent *entry;
    CupError err = CUP_OK;

    if (depth > SYSTEM_MAX_TREE_DEPTH) {
        return CUP_ERR_FILESYSTEM;
    }
    scan = dup(descriptor);
    if (scan < 0 || fcntl(scan, F_SETFD, FD_CLOEXEC) != 0) {
        if (scan >= 0) {
            close(scan);
        }
        return CUP_ERR_FILESYSTEM;
    }
    directory = fdopendir(scan);
    if (directory == NULL) {
        close(scan);
        return CUP_ERR_FILESYSTEM;
    }
    errno = 0;
    while ((entry = readdir(directory)) != NULL) {
        struct stat observed;
        char child[MAX_PATH_LEN];
        SystemPathKind kind;

        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        if (fstatat(descriptor, entry->d_name, &observed, AT_SYMLINK_NOFOLLOW) != 0) {
            err = CUP_ERR_FILESYSTEM;
            break;
        }
        err = path_join(child, sizeof(child), display, entry->d_name);
        if (err != CUP_OK) {
            break;
        }
        kind = path_kind_from_mode(observed.st_mode);
        if (system_test_crosses_boundary(entry->d_name, observed.st_dev, root_device)) {
            fprintf(stderr,
                    "Error: refusing to cross a filesystem boundary at '%s'.\n",
                    child);
            err = CUP_ERR_FILESYSTEM;
            break;
        }
        if (kind == SYSTEM_PATH_DIRECTORY) {
            int child_fd;
            struct stat opened;

            child_fd = openat(descriptor,
                              entry->d_name,
                              O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (child_fd < 0 || fstat(child_fd, &opened) != 0 ||
                !stat_identity_equal(&observed, &opened)) {
                if (child_fd >= 0) {
                    close(child_fd);
                }
                err = CUP_ERR_FILESYSTEM;
                break;
            }
            err = walk_directory_fd(
                child_fd, child, root_device, depth + 1u, callback, userdata);
            (void)close(child_fd);
            if (err != CUP_OK) {
                break;
            }
        }

        {
            SystemPathIdentity identity;

            identity_from_stat(&observed, &identity);
            err = callback(child, kind, &identity, userdata);
        }
        if (err != CUP_OK) {
            break;
        }
        errno = 0;
    }
    if (err == CUP_OK && errno != 0) {
        err = CUP_ERR_FILESYSTEM;
    }
    (void)closedir(directory);
    return err;
}

CupError system_walk_directory(const char *path,
                               SystemDirectoryCallback callback,
                               void *userdata) {
    int descriptor = -1;
    int missing = 0;
    struct stat root_info;
    CupError err;

    if (callback == NULL || text_is_empty(path)) {
        return CUP_ERR_INVALID_INPUT;
    }
    err = open_directory_path_no_follow_status(path, &descriptor, &missing);
    if (err != CUP_OK || missing) {
        return err;
    }
    if (fstat(descriptor, &root_info) != 0) {
        close(descriptor);
        return CUP_ERR_FILESYSTEM;
    }
    err = walk_directory_fd(descriptor, path, root_info.st_dev, 0u, callback, userdata);
    (void)close(descriptor);
    return err;
}

/* Nonblocking advisory locks tied to the acquired open-file description. */
static CupError lock_acquire_common(SystemLock *lock,
                                    const char *path,
                                    SystemLockMode mode,
                                    int create) {
    char entry[MAX_PATH_LEN];
    struct stat info;
    int operation;
    int flags;
    int parent_fd = -1;
    int fd;
    CupError err;

    if (lock == NULL || lock->active || text_is_empty(path) ||
        (mode != SYSTEM_LOCK_SHARED && mode != SYSTEM_LOCK_EXCLUSIVE) ||
        (create != 0 && create != 1)) {
        return CUP_ERR_INVALID_INPUT;
    }

    memset(lock, 0, sizeof(*lock));
    err = open_parent_no_follow(path, &parent_fd, entry, sizeof(entry));
    if (err != CUP_OK) {
        return err;
    }
    flags = (mode == SYSTEM_LOCK_SHARED ? O_RDONLY : O_RDWR) | O_NOFOLLOW | O_CLOEXEC;
    if (create) {
        flags |= O_CREAT;
    }
    fd = openat(parent_fd, entry, flags, 0644);
    if (fd < 0) {
        close(parent_fd);
        return CUP_ERR_FILESYSTEM;
    }
    if (fstat(fd, &info) != 0 || !S_ISREG(info.st_mode)) {
        close(fd);
        close(parent_fd);
        return CUP_ERR_FILESYSTEM;
    }
    (void)close(parent_fd);

    operation = (mode == SYSTEM_LOCK_EXCLUSIVE ? LOCK_EX : LOCK_SH) | LOCK_NB;
    if (flock(fd, operation) != 0) {
        int lock_errno = errno;

        close(fd);
        if (lock_errno == EWOULDBLOCK || lock_errno == EAGAIN) {
            return CUP_ERR_LOCK;
        }
        return CUP_ERR_FILESYSTEM;
    }

    lock->handle = fd;
    lock->mode = mode;
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
    struct stat info;

    if (identity == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    memset(identity, 0, sizeof(*identity));
    if (lock == NULL || !lock->active) {
        return CUP_ERR_INVALID_INPUT;
    }
    if (fstat((int)lock->handle, &info) != 0) {
        return CUP_ERR_FILESYSTEM;
    }

    identity_from_stat(&info, identity);
    return CUP_OK;
}

CupError system_lock_read(const SystemLock *lock,
                          void *buffer,
                          size_t capacity,
                          size_t *size) {
    size_t total = 0;

    if (size == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    *size = 0;
    if (lock == NULL || !lock->active || buffer == NULL || capacity == 0) {
        return CUP_ERR_INVALID_INPUT;
    }

    while (total < capacity) {
        ssize_t count;

        do {
            count = pread((int)lock->handle,
                          (unsigned char *)buffer + total,
                          capacity - total,
                          (off_t)total);
        } while (count < 0 && errno == EINTR);
        if (count < 0) {
            return CUP_ERR_FILESYSTEM;
        }
        if (count == 0) {
            break;
        }
        total += (size_t)count;
    }

    *size = total;
    return CUP_OK;
}

void system_lock_release(SystemLock *lock) {
    if (lock == NULL || !lock->active) {
        return;
    }

    (void)flock((int)lock->handle, LOCK_UN);
    (void)close((int)lock->handle);
    lock->handle = -1;
    lock->mode = SYSTEM_LOCK_SHARED;
    lock->active = 0;
}

CupError system_handoff_active(int *active) {
    if (active == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    *active = 0;
    return CUP_OK;
}
