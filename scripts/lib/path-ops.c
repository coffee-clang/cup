#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include "constants.h"
#include "path.h"
#include "system.h"
#include "text.h"

#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#include <bcrypt.h>
#include <io.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#endif

#define CUP_PATH_OPS_PROTOCOL 2
#define CUP_PATH_OPS_MAX_DEPTH 128u
#define CUP_PATH_OPS_UNIQUE_ATTEMPTS 256u
#define CUP_PATH_OPS_UNIQUE_CHARS 6u
#define CUP_BUILD_ROOT_MARKER ".cup-build-root"

static const char CUP_BUILD_ROOT_MARKER_CONTENT[] =
    "format=1\n"
    "product=coffee-clang/cup\n"
    "kind=build-root\n"
    "layout=1\n";

static char pending_temporary[MAX_PATH_LEN];

static void remove_pending_temporary(void) {
    if (pending_temporary[0] != '\0') {
        (void)system_remove_file(pending_temporary);
        pending_temporary[0] = '\0';
    }
}

static _Noreturn void fail_message(const char *format, ...) {
    va_list arguments;

    remove_pending_temporary();
    fputs("path ops: ", stderr);
    va_start(arguments, format);
    vfprintf(stderr, format, arguments);
    va_end(arguments);
    fputc('\n', stderr);
    exit(1);
}

static _Noreturn void fail_system(const char *operation,
                                  const char *path,
                                  CupError error) {
    fail_message("%s '%s' failed with cup error %d", operation, path, (int)error);
}

#ifdef CUP_PATH_OPS_TESTING
static void test_pause(const char *point) {
    const char *expected = getenv("CUP_PATH_OPS_TEST_POINT");
    const char *ready = getenv("CUP_PATH_OPS_TEST_READY");
    const char *resume = getenv("CUP_PATH_OPS_TEST_CONTINUE");
#if !defined(_WIN32)
    struct timespec delay = {0, 10000000L};
#endif
    unsigned int attempt;
    FILE *file;

    if (expected == NULL || strcmp(expected, point) != 0) {
        return;
    }
    if (ready == NULL || resume == NULL) {
        fail_message("test pause requires ready and continue paths");
    }

    file = fopen(ready, "wx");
    if (file == NULL) {
        fail_message("could not publish test pause at %s", point);
    }
    fclose(file);

    for (attempt = 0; attempt < 3000; ++attempt) {
#if defined(_WIN32)
        if (_access(resume, 0) == 0) {
            return;
        }
#else
        if (access(resume, F_OK) == 0) {
            return;
        }
#endif
        if (errno != ENOENT) {
            fail_message("could not inspect test continuation at %s", point);
        }
#if defined(_WIN32)
        Sleep(10);
#else
        nanosleep(&delay, NULL);
#endif
    }
    fail_message("timed out waiting for test continuation at %s", point);
}
#else
static void test_pause(const char *point) {
    (void)point;
}
#endif

static bool path_is_clean_absolute(const char *path) {
    const char *component;
    const char *cursor;
    size_t length;

    if (path == NULL) {
        return false;
    }
    length = strlen(path);

#if defined(_WIN32)
    if (length >= 3 &&
        ((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z')) &&
        path[1] == ':' && path[2] == '/') {
        cursor = path + 3;
        if (length == 3) {
            return true;
        }
    } else if (length >= 5 && path[0] == '/' && path[1] == '/') {
        cursor = path + 2;
    } else {
        return false;
    }
#else
    if (path[0] != '/' || path[1] == '\0') {
        return false;
    }
    cursor = path + 1;
#endif

    if (path[length - 1] == '/' ||
        strchr(path, '\\') != NULL || strchr(path, '\n') != NULL ||
        strchr(path, '\r') != NULL) {
        return false;
    }

    while (*cursor != '\0') {
        component = cursor;
        while (*cursor != '\0' && *cursor != '/') {
#if defined(_WIN32)
            if (*cursor == ':') {
                return false;
            }
#endif
            cursor++;
        }

        length = (size_t)(cursor - component);
        if (length == 0 || (length == 1 && component[0] == '.') ||
            (length == 2 && component[0] == '.' && component[1] == '.')) {
            return false;
        }
        if (*cursor == '/') {
            cursor++;
        }
    }
    return true;
}

static void require_clean_path(const char *path) {
    if (!path_is_clean_absolute(path)) {
        fail_message("path is not an absolute clean path: %s",
                     path == NULL ? "(null)" : path);
    }
}

static void require_safe_build_root_path(const char *root) {
    char home[MAX_PATH_LEN];
    CupError err;

    require_clean_path(root);
    err = system_get_home_dir(home, sizeof(home));
    if (err != CUP_OK) {
        fail_system("resolve user home", root, err);
    }
#if defined(_WIN32)
    if (_stricmp(root, home) == 0) {
#else
    if (strcmp(root, home) == 0) {
#endif
        fail_message("build root must not be the user home directory: %s", root);
    }
}

static unsigned int parse_mode(const char *value) {
    unsigned int mode = 0;
    size_t index;

    if (value == NULL || strlen(value) != 4 || value[0] != '0') {
        fail_message("invalid file mode: %s", value == NULL ? "(null)" : value);
    }
    for (index = 1; index < 4; ++index) {
        if (value[index] < '0' || value[index] > '7') {
            fail_message("invalid file mode: %s", value);
        }
        mode = mode * 8u + (unsigned int)(value[index] - '0');
    }
    return mode;
}

static bool path_contains(const char *parent, const char *child) {
    char child_prefix[MAX_PATH_LEN];
    size_t length = strlen(parent);

    if (strlen(child) <= length || child[length] != '/' || length >= sizeof(child_prefix)) {
        return false;
    }
    memcpy(child_prefix, child, length);
    child_prefix[length] = '\0';
    return path_equal(parent, child_prefix) != 0;
}

static void check_directory(const char *path, int allow_missing) {
    CupError err;

    require_clean_path(path);
    err = system_check_directory_chain(path, allow_missing);
    if (err != CUP_OK) {
        fail_message("path contains a symlink or reparse point, a non-directory component, "
                     "or a disallowed missing component: %s",
                     path);
    }
}

static void ensure_directory(const char *path) {
    CupError err;

    require_clean_path(path);
    err = system_make_directory_chain(path);
    if (err != CUP_OK) {
        fail_message("path contains a symlink or reparse point or a non-directory component: %s",
                     path);
    }
}

static void create_directory_exclusive(const char *path) {
    SystemCommitState state = SYSTEM_COMMIT_NOT_APPLIED;
    CupError err;

    require_clean_path(path);
    err = system_create_directory_exclusive(path, 0755u, &state);
    if (err != CUP_OK || state != SYSTEM_COMMIT_DURABLE) {
        if (state == SYSTEM_COMMIT_APPLIED) {
            (void)system_remove_directory(path);
        }
        fail_system("create exclusive directory",
                    path,
                    err == CUP_OK ? CUP_ERR_COMMIT : err);
    }
}

static void fill_unique_suffix(char *suffix, size_t count) {
    static const char alphabet[] = "abcdefghijklmnopqrstuvwxyz234567";
    unsigned char random_bytes[CUP_PATH_OPS_UNIQUE_CHARS];
    size_t offset = 0;
#if !defined(_WIN32)
    int descriptor;
#endif

    if (suffix == NULL || count == 0 || count > sizeof(random_bytes)) {
        fail_message("invalid unique suffix request");
    }

#if defined(_WIN32)
    if (count > ULONG_MAX ||
        BCryptGenRandom(NULL,
                        random_bytes,
                        (ULONG)count,
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
        fail_message("could not read system random source");
    }
#else
    descriptor = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (descriptor < 0) {
        fail_message("could not open system random source");
    }
    while (offset < count) {
        ssize_t result = read(descriptor, random_bytes + offset, count - offset);

        if (result > 0) {
            offset += (size_t)result;
            continue;
        }
        if (result < 0 && errno == EINTR) {
            continue;
        }
        (void)close(descriptor);
        fail_message("could not read system random source");
    }
    if (close(descriptor) != 0) {
        fail_message("could not close system random source");
    }
#endif

    for (offset = 0; offset < count; ++offset) {
        suffix[offset] = alphabet[random_bytes[offset] & 31u];
    }
}

static void build_unique_candidate(const char *template_path,
                                   char *path,
                                   size_t path_size) {
    char suffix[CUP_PATH_OPS_UNIQUE_CHARS];
    size_t length;

    if (template_path == NULL || path == NULL || path_size == 0) {
        fail_message("invalid unique path template");
    }
    length = strlen(template_path);
    if (length < CUP_PATH_OPS_UNIQUE_CHARS ||
        strcmp(template_path + length - CUP_PATH_OPS_UNIQUE_CHARS, "XXXXXX") != 0 ||
        text_copy(path, path_size, template_path) != CUP_OK) {
        fail_message("unique path template must end in XXXXXX: %s", template_path);
    }

    fill_unique_suffix(suffix, sizeof(suffix));
    memcpy(path + length - sizeof(suffix), suffix, sizeof(suffix));
}

static void create_unique_directory_path(const char *template_path,
                                         unsigned int mode,
                                         char *path,
                                         size_t path_size) {
    char parent[MAX_PATH_LEN];
    unsigned int attempt;

    require_clean_path(template_path);
    if (path_parent(parent, sizeof(parent), template_path) != CUP_OK) {
        fail_message("could not resolve unique directory parent: %s", template_path);
    }
    check_directory(parent, 0);
    test_pause("before-mkdir-unique");

    for (attempt = 0; attempt < CUP_PATH_OPS_UNIQUE_ATTEMPTS; ++attempt) {
        SystemCommitState state = SYSTEM_COMMIT_NOT_APPLIED;
        CupError err;

        build_unique_candidate(template_path, path, path_size);
        err = system_create_directory_exclusive(path, (unsigned int)mode, &state);
        if (err == CUP_ERR_LOCK && state == SYSTEM_COMMIT_NOT_APPLIED) {
            continue;
        }
        if (err == CUP_OK && state == SYSTEM_COMMIT_DURABLE) {
            return;
        }
        if (state == SYSTEM_COMMIT_APPLIED) {
            (void)system_remove_directory(path);
        }
        fail_system("create unique directory",
                    template_path,
                    err == CUP_OK ? CUP_ERR_COMMIT : err);
    }

    fail_message("could not allocate a unique directory from %s", template_path);
}

static void create_unique_directory(const char *template_path, unsigned int mode) {
    char path[MAX_PATH_LEN];

    create_unique_directory_path(template_path, mode, path, sizeof(path));
    printf("%s\n", path);
}

static void check_regular_file(const char *path) {
    SystemPathIdentity identity;
    FILE *file = NULL;
    uint64_t size = 0;
    int missing = 0;
    CupError err;

    require_clean_path(path);
    memset(&identity, 0, sizeof(identity));
    err = system_open_regular_file(path, &file, &identity, &size, &missing);
    if (err != CUP_OK || missing || file == NULL || !identity.valid ||
        identity.kind != SYSTEM_PATH_REGULAR_FILE) {
        if (file != NULL) {
            (void)fclose(file);
        }
        fail_message("not a no-follow regular file: %s", path);
    }
    if (fclose(file) != 0) {
        fail_message("could not close checked regular file: %s", path);
    }
}

static CupError safe_tree_entry(const char *path,
                                SystemPathKind kind,
                                const SystemPathIdentity *identity,
                                void *userdata) {
    (void)path;
    (void)identity;
    (void)userdata;

    return kind == SYSTEM_PATH_REGULAR_FILE || kind == SYSTEM_PATH_DIRECTORY
               ? CUP_OK
               : CUP_ERR_FILESYSTEM;
}

static void check_tree(const char *path) {
    SystemPathKind kind;
    CupError err;

    require_clean_path(path);
    err = system_get_path_kind(path, &kind);
    if (err != CUP_OK || kind != SYSTEM_PATH_DIRECTORY) {
        fail_message("not a directory tree: %s", path);
    }

    err = system_walk_directory(path, safe_tree_entry, NULL);
    if (err != CUP_OK) {
        fail_message("tree contains a link, special entry, or filesystem boundary: %s", path);
    }
}

static void remove_tree(const char *path) {
    SystemPathKind kind;
    CupError err;

    require_clean_path(path);
    err = system_get_path_kind(path, &kind);
    if (err != CUP_OK) {
        fail_system("inspect directory tree", path, err);
    }
    if (kind == SYSTEM_PATH_MISSING) {
        return;
    }
    if (kind != SYSTEM_PATH_DIRECTORY) {
        fail_message("removal target is not a directory tree: %s", path);
    }

    err = system_remove_tree(path, NULL);
    if (err != CUP_OK) {
        fail_system("remove directory tree", path, err);
    }
}

static void remove_empty_directory(const char *path) {
    CupError err;

    require_clean_path(path);
    err = system_remove_directory(path);
    if (err != CUP_OK) {
        fail_system("remove empty directory", path, err);
    }
}

static void remove_file(const char *path) {
    SystemPathIdentity identity;
    CupError err;

    require_clean_path(path);
    memset(&identity, 0, sizeof(identity));
    err = system_get_path_identity(path, &identity);
    if (err != CUP_OK) {
        fail_system("inspect removal file", path, err);
    }
    if (!identity.valid) {
        return;
    }
    if (identity.kind != SYSTEM_PATH_REGULAR_FILE) {
        fail_message("removal target is not a regular file: %s", path);
    }

    err = system_remove_file_if_identity(path, &identity);
    if (err != CUP_OK) {
        fail_system("remove file", path, err);
    }
}

typedef enum {
    FILE_COMMIT_REPLACE = 0,
    FILE_COMMIT_IF_DIFFERENT,
    FILE_COMMIT_NO_REPLACE
} FileCommitPolicy;

static int files_equal(const char *left_path, const char *right_path) {
    SystemPathIdentity left_identity;
    SystemPathIdentity right_identity;
    FILE *left = NULL;
    FILE *right = NULL;
    unsigned char left_buffer[16384];
    unsigned char right_buffer[16384];
    uint64_t left_size = 0;
    uint64_t right_size = 0;
    int left_missing = 0;
    int right_missing = 0;
    int equal = 1;
    CupError err;

    err = system_open_regular_file(left_path,
                                   &left,
                                   &left_identity,
                                   &left_size,
                                   &left_missing);
    if (err != CUP_OK || left_missing) {
        fail_system("open comparison file", left_path, err);
    }
    err = system_open_regular_file(right_path,
                                   &right,
                                   &right_identity,
                                   &right_size,
                                   &right_missing);
    if (err != CUP_OK || right_missing) {
        (void)fclose(left);
        fail_system("open comparison file", right_path, err);
    }
    if (left_size != right_size) {
        equal = 0;
    }

    while (equal) {
        size_t left_count = fread(left_buffer, 1, sizeof(left_buffer), left);
        size_t right_count = fread(right_buffer, 1, sizeof(right_buffer), right);

        if (ferror(left) || ferror(right)) {
            fclose(left);
            fclose(right);
            fail_message("could not compare destination files");
        }
        if (left_count != right_count ||
            (left_count > 0 && memcmp(left_buffer, right_buffer, left_count) != 0)) {
            equal = 0;
        }
        if (left_count == 0 || right_count == 0) {
            break;
        }
    }

    {
        int left_close_error = fclose(left);
        int right_close_error = fclose(right);

        if (left_close_error != 0 || right_close_error != 0) {
            fail_message("could not close compared destination files");
        }
    }
    return equal;
}

static FILE *create_temporary_file(const char *destination,
                                   unsigned int mode,
                                   char *path,
                                   size_t path_size) {
    char parent[MAX_PATH_LEN];
    char template_path[MAX_PATH_LEN];
    FILE *file = NULL;
    unsigned int attempt;

    if (path_parent(parent, sizeof(parent), destination) != CUP_OK) {
        fail_message("could not resolve destination parent: %s", destination);
    }
    ensure_directory(parent);
    if (text_format(template_path,
                    sizeof(template_path),
                    "%s/.cup-path-ops.XXXXXX",
                    parent) != CUP_OK) {
        fail_message("temporary destination path is too long: %s", destination);
    }

    for (attempt = 0; attempt < CUP_PATH_OPS_UNIQUE_ATTEMPTS; ++attempt) {
        CupError err;

        build_unique_candidate(template_path, path, path_size);
        err = system_create_file_exclusive(path, &file);
        if (err == CUP_ERR_LOCK) {
            continue;
        }
        if (err != CUP_OK) {
            fail_system("create temporary destination", destination, err);
        }
        break;
    }
    if (file == NULL) {
        fail_message("could not allocate temporary destination: %s", destination);
    }
    if (text_copy(pending_temporary, sizeof(pending_temporary), path) != CUP_OK) {
        fclose(file);
        fail_message("temporary destination path is too long: %s", destination);
    }
#if defined(_WIN32)
    (void)mode;
#else
    if (fchmod(fileno(file), (mode_t)mode) != 0) {
        fclose(file);
        fail_message("could not set temporary file mode: %s", destination);
    }
#endif
    return file;
}

static void write_stream(FILE *source, FILE *destination, const char *display) {
    unsigned char buffer[32768];

    for (;;) {
        size_t count = fread(buffer, 1, sizeof(buffer), source);

        if (count > 0 && fwrite(buffer, 1, count, destination) != count) {
            fail_message("could not write temporary file: %s", display);
        }
        if (count < sizeof(buffer)) {
            if (ferror(source)) {
                fail_message("could not read source file: %s", display);
            }
            break;
        }
    }
}

static void commit_temporary(const char *destination, FileCommitPolicy policy) {
    SystemPathIdentity destination_identity;
    SystemCommitState state = SYSTEM_COMMIT_NOT_APPLIED;
    CupError err;

    memset(&destination_identity, 0, sizeof(destination_identity));
    err = system_get_path_identity(destination, &destination_identity);
    if (err != CUP_OK) {
        fail_system("inspect destination file", destination, err);
    }
    if (destination_identity.valid &&
        destination_identity.kind != SYSTEM_PATH_REGULAR_FILE) {
        fail_message("destination is not a regular file: %s", destination);
    }
    if (policy == FILE_COMMIT_NO_REPLACE && destination_identity.valid) {
        fail_message("destination already exists: %s", destination);
    }
    if (policy == FILE_COMMIT_IF_DIFFERENT && destination_identity.valid &&
        files_equal(pending_temporary, destination)) {
        err = system_remove_file(pending_temporary);
        if (err != CUP_OK) {
            fail_system("remove unchanged temporary file", destination, err);
        }
        pending_temporary[0] = '\0';
        return;
    }

    if (!destination_identity.valid) {
        err = system_move_path(pending_temporary, destination, &state);
    } else {
        err = system_replace_file_if_identity(pending_temporary,
                                              destination,
                                              &destination_identity,
                                              &state);
    }
    if (err != CUP_OK) {
        if (state == SYSTEM_COMMIT_NOT_APPLIED) {
            remove_pending_temporary();
        }
        fail_system("publish destination file", destination, err);
    }
    if (state != SYSTEM_COMMIT_DURABLE) {
        fail_message("destination publication durability is uncertain: %s", destination);
    }

    pending_temporary[0] = '\0';
}

static void copy_stream_to_destination(FILE *source,
                                       const char *destination,
                                       unsigned int mode,
                                       FileCommitPolicy policy) {
    char temporary[MAX_PATH_LEN];
    FILE *output;

    CupError sync_error;
    int close_error;

    output = create_temporary_file(destination, mode, temporary, sizeof(temporary));
    write_stream(source, output, destination);

    sync_error = system_sync_file(output);
    close_error = fclose(output);
    if (sync_error != CUP_OK || close_error != 0) {
        fail_message("could not finalize temporary file: %s", destination);
    }

    commit_temporary(destination, policy);
}

static void copy_file(const char *source_path,
                      const char *destination,
                      unsigned int mode,
                      FileCommitPolicy policy) {
    SystemPathIdentity identity;
    FILE *source = NULL;
    uint64_t source_size = 0;
    int missing = 0;
    CupError err;

    require_clean_path(source_path);
    require_clean_path(destination);
    err = system_open_regular_file(
        source_path, &source, &identity, &source_size, &missing);
    if (err != CUP_OK || missing) {
        fail_system("open source file", source_path, err);
    }

    copy_stream_to_destination(source, destination, mode, policy);
    (void)fclose(source);
}

static void write_stdin(const char *destination,
                        unsigned int mode,
                        FileCommitPolicy policy) {
    require_clean_path(destination);
    copy_stream_to_destination(stdin, destination, mode, policy);
}

typedef struct {
    const char *destination;
    unsigned int depth;
} CopyTreeContext;

static CupError copy_tree_entry(const char *source_path,
                                SystemPathKind kind,
                                const SystemPathIdentity *identity,
                                void *userdata);

static void copy_observed_file(const char *source_path,
                               const SystemPathIdentity *expected_identity,
                               const char *destination_path) {
    SystemPathIdentity opened_identity;
    FILE *source = NULL;
    uint64_t source_size = 0;
    int missing = 0;
    int executable = 0;
    CupError err;
    unsigned int mode;

    if (expected_identity == NULL || !expected_identity->valid ||
        expected_identity->kind != SYSTEM_PATH_REGULAR_FILE) {
        fail_message("invalid observed source identity: %s", source_path);
    }

    err = system_open_regular_file(
        source_path, &source, &opened_identity, &source_size, &missing);
    if (err != CUP_OK || missing ||
        !system_path_identity_equal(expected_identity, &opened_identity)) {
        if (source != NULL) {
            fclose(source);
        }
        fail_message("source file identity changed during copy: %s", source_path);
    }
    err = system_file_is_executable(source, source_path, &executable);
    if (err != CUP_OK) {
        fclose(source);
        fail_system("inspect opened source file", source_path, err);
    }

    mode = executable ? 0755u : 0644u;
    copy_stream_to_destination(source, destination_path, mode, FILE_COMMIT_REPLACE);
    (void)fclose(source);
}

static void copy_tree_directory(const char *source,
                                const SystemPathIdentity *expected_identity,
                                const char *destination,
                                unsigned int depth) {
    SystemPathIdentity before;
    SystemPathIdentity after;
    CopyTreeContext context;
    CupError err;

    if (depth > CUP_PATH_OPS_MAX_DEPTH) {
        fail_message("tree exceeds maximum depth: %s", source);
    }

    memset(&before, 0, sizeof(before));
    err = system_get_path_identity(source, &before);
    if (err != CUP_OK || !before.valid || before.kind != SYSTEM_PATH_DIRECTORY ||
        (expected_identity != NULL &&
         !system_path_identity_equal(expected_identity, &before))) {
        fail_message("source tree changed before copy: %s", source);
    }

    context.destination = destination;
    context.depth = depth;
    err = system_list_directory(source, copy_tree_entry, &context);
    if (err != CUP_OK) {
        fail_system("copy directory tree", source, err);
    }

    memset(&after, 0, sizeof(after));
    err = system_get_path_identity(source, &after);
    if (err != CUP_OK || !system_path_identity_equal(&before, &after)) {
        fail_message("source tree identity changed during copy: %s", source);
    }
}

static CupError copy_tree_entry(const char *source_path,
                                SystemPathKind kind,
                                const SystemPathIdentity *identity,
                                void *userdata) {
    CopyTreeContext *context = userdata;
    const char *name = path_last_segment(source_path);
    char destination_path[MAX_PATH_LEN];

    if (context == NULL || identity == NULL || !identity->valid ||
        identity->kind != kind || name == NULL) {
        return CUP_ERR_INCONSISTENT_STATE;
    }
    if (path_join(destination_path,
                  sizeof(destination_path),
                  context->destination,
                  name) != CUP_OK) {
        return CUP_ERR_BUFFER_TOO_SMALL;
    }

    if (kind == SYSTEM_PATH_DIRECTORY) {
        CupError err = system_make_directory_chain(destination_path);

        if (err != CUP_OK) {
            return err;
        }
        copy_tree_directory(
            source_path, identity, destination_path, context->depth + 1u);
        return CUP_OK;
    }
    if (kind == SYSTEM_PATH_REGULAR_FILE) {
        copy_observed_file(source_path, identity, destination_path);
        return CUP_OK;
    }
    return CUP_ERR_FILESYSTEM;
}

static void copy_tree(const char *source, const char *destination) {
    SystemPathKind destination_kind;
    CupError err;

    require_clean_path(source);
    require_clean_path(destination);
    if (path_equal(source, destination) || path_contains(source, destination) ||
        path_contains(destination, source)) {
        fail_message("tree copy paths must not overlap: %s -> %s", source, destination);
    }

    check_tree(source);
    err = system_get_path_kind(destination, &destination_kind);
    if (err != CUP_OK || destination_kind != SYSTEM_PATH_DIRECTORY) {
        fail_message("tree copy destination is not a directory: %s", destination);
    }

    copy_tree_directory(source, NULL, destination, 0u);
}

static void move_entry(const char *source, const char *destination) {
    SystemCommitState state = SYSTEM_COMMIT_NOT_APPLIED;
    CupError err;

    require_clean_path(source);
    require_clean_path(destination);
    {
        char parent[MAX_PATH_LEN];

        if (path_parent(parent, sizeof(parent), destination) != CUP_OK) {
            fail_message("could not resolve move destination parent: %s", destination);
        }
        ensure_directory(parent);
    }

    err = system_move_path(source, destination, &state);
    if (err == CUP_ERR_INCONSISTENT_STATE && state == SYSTEM_COMMIT_NOT_APPLIED) {
        fail_message("move source identity changed before publication: %s", source);
    }
    if (err != CUP_OK) {
        fail_system("move entry", destination, err);
    }
    if (state != SYSTEM_COMMIT_DURABLE) {
        fail_message("move durability is uncertain: %s", destination);
    }
}

typedef struct {
    SystemLock lock;
    SystemPathIdentity root_identity;
    SystemPathIdentity marker_identity;
    char marker[MAX_PATH_LEN];
} BuildRootLock;

static int locked_root_unchanged(const char *root, const BuildRootLock *locked) {
    SystemPathIdentity root_identity;
    SystemPathIdentity marker_identity;
    CupError err;

    memset(&root_identity, 0, sizeof(root_identity));
    memset(&marker_identity, 0, sizeof(marker_identity));
    err = system_get_path_identity(root, &root_identity);
    if (err == CUP_OK) {
        err = system_get_path_identity(locked->marker, &marker_identity);
    }
    return err == CUP_OK &&
           system_path_identity_equal(&locked->root_identity, &root_identity) &&
           system_path_identity_equal(&locked->marker_identity, &marker_identity);
}

static void require_locked_root_unchanged(const char *root,
                                          const BuildRootLock *locked) {
    if (!locked_root_unchanged(root, locked)) {
        fail_message("build root identity changed while locked: %s", root);
    }
}

static void lock_build_root(const char *root, BuildRootLock *locked) {
    unsigned char marker_buffer[256];
    size_t marker_size = 0;
    CupError err;

    require_safe_build_root_path(root);
    memset(locked, 0, sizeof(*locked));
    if (path_join(locked->marker,
                  sizeof(locked->marker),
                  root,
                  CUP_BUILD_ROOT_MARKER) != CUP_OK) {
        fail_message("build root marker path is too long: %s", root);
    }

    err = system_get_path_identity(root, &locked->root_identity);
    if (err != CUP_OK || !locked->root_identity.valid ||
        locked->root_identity.kind != SYSTEM_PATH_DIRECTORY) {
        fail_message("invalid build root: %s", root);
    }

    err = system_lock_acquire_existing(&locked->lock,
                                       locked->marker,
                                       SYSTEM_LOCK_EXCLUSIVE);
    if (err == CUP_ERR_LOCK) {
        fail_message("build root is busy: %s", root);
    }
    if (err != CUP_OK) {
        fail_system("lock build root", root, err);
    }
    if (system_lock_get_identity(&locked->lock, &locked->marker_identity) != CUP_OK) {
        fail_message("could not inspect locked build root marker: %s", root);
    }

    err = system_lock_read(&locked->lock,
                           marker_buffer,
                           sizeof(marker_buffer),
                           &marker_size);
    if (err != CUP_OK || marker_size != strlen(CUP_BUILD_ROOT_MARKER_CONTENT) ||
        memcmp(marker_buffer, CUP_BUILD_ROOT_MARKER_CONTENT, marker_size) != 0) {
        system_lock_release(&locked->lock);
        fail_message("invalid build root marker: %s", locked->marker);
    }
    test_pause("before-build-lock-identity-check");
    if (!locked_root_unchanged(root, locked)) {
        system_lock_release(&locked->lock);
        fail_message("build root identity changed while acquiring lock: %s", root);
    }
}

static void check_build_root(const char *root) {
    SystemPathIdentity root_before;
    SystemPathIdentity root_after;
    SystemPathIdentity marker_identity;
    SystemPathIdentity marker_after;
    char marker[MAX_PATH_LEN];
    unsigned char content[sizeof(CUP_BUILD_ROOT_MARKER_CONTENT)];
    FILE *file = NULL;
    uint64_t size = 0;
    int missing = 0;
    CupError err;
    size_t count;

    require_safe_build_root_path(root);
    memset(&root_before, 0, sizeof(root_before));
    memset(&root_after, 0, sizeof(root_after));
    memset(&marker_identity, 0, sizeof(marker_identity));
    memset(&marker_after, 0, sizeof(marker_after));

    err = system_get_path_identity(root, &root_before);
    if (err != CUP_OK || !root_before.valid ||
        root_before.kind != SYSTEM_PATH_DIRECTORY ||
        path_join(marker, sizeof(marker), root, CUP_BUILD_ROOT_MARKER) != CUP_OK) {
        fail_message("invalid build root: %s", root);
    }

    err = system_open_regular_file(
        marker, &file, &marker_identity, &size, &missing);
    if (err != CUP_OK || missing ||
        size != strlen(CUP_BUILD_ROOT_MARKER_CONTENT)) {
        if (file != NULL) {
            fclose(file);
        }
        fail_message("invalid build root marker: %s", marker);
    }
    count = fread(content, 1, sizeof(content), file);
    if (count != size || ferror(file) ||
        memcmp(content, CUP_BUILD_ROOT_MARKER_CONTENT, count) != 0 ||
        fgetc(file) != EOF) {
        fclose(file);
        fail_message("invalid build root marker: %s", marker);
    }
    if (fclose(file) != 0) {
        fail_message("could not close build root marker: %s", marker);
    }

    err = system_get_path_identity(marker, &marker_after);
    if (err == CUP_OK) {
        err = system_get_path_identity(root, &root_after);
    }
    if (err != CUP_OK ||
        !system_path_identity_equal(&marker_identity, &marker_after) ||
        !system_path_identity_equal(&root_before, &root_after)) {
        fail_message("build root identity changed during validation: %s", root);
    }
}

static void unlock_build_root(BuildRootLock *locked) {
    if (locked != NULL) {
        system_lock_release(&locked->lock);
    }
}

static void validate_build_root(const char *root) {
    BuildRootLock locked;

    check_build_root(root);
    lock_build_root(root, &locked);
    unlock_build_root(&locked);
}

static void prepare_build_root(const char *root) {
    SystemPathKind kind;
    SystemCommitState state = SYSTEM_COMMIT_NOT_APPLIED;
    char parent[MAX_PATH_LEN];
    char staging_template[MAX_PATH_LEN];
    char staging[MAX_PATH_LEN];
    char marker[MAX_PATH_LEN];
    FILE *file = NULL;
    CupError err;
    int staging_exists = 0;

    require_safe_build_root_path(root);
    err = system_get_path_kind(root, &kind);
    if (err != CUP_OK) {
        fail_system("inspect build root", root, err);
    }
    if (kind != SYSTEM_PATH_MISSING) {
        validate_build_root(root);
        return;
    }

    if (path_parent(parent, sizeof(parent), root) != CUP_OK) {
        fail_message("could not resolve build root parent: %s", root);
    }
    err = system_make_directory_chain(parent);
    if (err != CUP_OK) {
        fail_system("prepare build root parent", parent, err);
    }
    if (text_format(staging_template,
                    sizeof(staging_template),
                    "%s/.cup-build-root.XXXXXX",
                    parent) != CUP_OK) {
        fail_message("staged build root path is too long: %s", root);
    }
    create_unique_directory_path(
        staging_template, 0755, staging, sizeof(staging));
    staging_exists = 1;

    if (path_join(marker, sizeof(marker), staging, CUP_BUILD_ROOT_MARKER) != CUP_OK) {
        (void)system_remove_tree(staging, NULL);
        fail_message("staged build root marker path is too long: %s", root);
    }
    err = system_create_file_exclusive(marker, &file);
    if (err == CUP_OK &&
        fwrite(CUP_BUILD_ROOT_MARKER_CONTENT,
               1,
               strlen(CUP_BUILD_ROOT_MARKER_CONTENT),
               file) != strlen(CUP_BUILD_ROOT_MARKER_CONTENT)) {
        err = CUP_ERR_FILESYSTEM;
    }
#if !defined(_WIN32)
    if (err == CUP_OK && fchmod(fileno(file), 0644) != 0) {
        err = CUP_ERR_FILESYSTEM;
    }
#endif
    if (err == CUP_OK) {
        err = system_sync_file(file);
    }
    if (file != NULL && fclose(file) != 0 && err == CUP_OK) {
        err = CUP_ERR_FILESYSTEM;
    }
    if (err == CUP_OK) {
        err = system_sync_parent_directory(marker);
    }
    if (err != CUP_OK) {
        (void)system_remove_tree(staging, NULL);
        fail_system("initialize staged build root", root, err);
    }

    err = system_move_path(staging, root, &state);
    if (err == CUP_OK && state == SYSTEM_COMMIT_DURABLE) {
        staging_exists = 0;
    }
    if (staging_exists) {
        (void)system_remove_tree(staging, NULL);
    }
    if (err != CUP_OK || state != SYSTEM_COMMIT_DURABLE) {
        /* A concurrent creator is acceptable only when our staged root was not published. */
        if (state == SYSTEM_COMMIT_NOT_APPLIED) {
            err = system_get_path_kind(root, &kind);
            if (err == CUP_OK && kind == SYSTEM_PATH_DIRECTORY) {
                validate_build_root(root);
                return;
            }
        }
        fail_system("publish build root",
                    root,
                    state == SYSTEM_COMMIT_APPLIED || err == CUP_OK
                        ? CUP_ERR_COMMIT
                        : err);
    }

    validate_build_root(root);
}

#if defined(_WIN32)
#define CUP_WINDOWS_COMMAND_LINE_CAP 32767u

static wchar_t *windows_wide_argument(const char *argument) {
    int count;
    wchar_t *wide;

    if (argument == NULL) {
        fail_message("locked build command contains a null argument");
    }
    count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, argument, -1, NULL, 0);
    if (count <= 0) {
        fail_message("locked build command contains invalid UTF-8");
    }
    wide = malloc((size_t)count * sizeof(*wide));
    if (wide == NULL) {
        fail_message("could not allocate locked build command argument");
    }
    if (MultiByteToWideChar(CP_UTF8,
                            MB_ERR_INVALID_CHARS,
                            argument,
                            -1,
                            wide,
                            count) != count) {
        free(wide);
        fail_message("could not convert locked build command argument");
    }
    return wide;
}

static void windows_command_append(wchar_t *line,
                                   size_t capacity,
                                   size_t *length,
                                   wchar_t value) {
    if (*length + 1 >= capacity) {
        fail_message("locked build command line is too long");
    }
    line[(*length)++] = value;
}

/* Encode one argv element using the Windows C-runtime command-line rules.
 * Quoting every element keeps empty strings, whitespace, quotes and trailing
 * backslashes lossless when CreateProcess hands the command line to the child. */
static void windows_command_append_argument(wchar_t *line,
                                            size_t capacity,
                                            size_t *length,
                                            const wchar_t *argument) {
    const wchar_t *cursor = argument;

    windows_command_append(line, capacity, length, L'"');
    for (;;) {
        size_t backslashes = 0;

        while (*cursor == L'\\') {
            ++backslashes;
            ++cursor;
        }
        if (*cursor == L'\0') {
            while (backslashes-- > 0) {
                windows_command_append(line, capacity, length, L'\\');
                windows_command_append(line, capacity, length, L'\\');
            }
            break;
        }
        if (*cursor == L'"') {
            size_t count = backslashes * 2u + 1u;

            while (count-- > 0) {
                windows_command_append(line, capacity, length, L'\\');
            }
            windows_command_append(line, capacity, length, L'"');
            ++cursor;
            continue;
        }
        while (backslashes-- > 0) {
            windows_command_append(line, capacity, length, L'\\');
        }
        windows_command_append(line, capacity, length, *cursor++);
    }
    windows_command_append(line, capacity, length, L'"');
}

static wchar_t *windows_build_command_line(char *const command[]) {
    wchar_t *line;
    size_t length = 0;
    size_t index;

    line = calloc(CUP_WINDOWS_COMMAND_LINE_CAP, sizeof(*line));
    if (line == NULL) {
        fail_message("could not allocate locked build command line");
    }
    for (index = 0; command[index] != NULL; ++index) {
        wchar_t *argument = windows_wide_argument(command[index]);

        if (index > 0) {
            windows_command_append(line,
                                   CUP_WINDOWS_COMMAND_LINE_CAP,
                                   &length,
                                   L' ');
        }
        windows_command_append_argument(line,
                                        CUP_WINDOWS_COMMAND_LINE_CAP,
                                        &length,
                                        argument);
        free(argument);
    }
    line[length] = L'\0';
    return line;
}

static int windows_run_command(char *const command[]) {
    STARTUPINFOW startup;
    PROCESS_INFORMATION process;
    wchar_t *command_line;
    DWORD wait_result;
    DWORD exit_code;

    memset(&startup, 0, sizeof(startup));
    memset(&process, 0, sizeof(process));
    startup.cb = sizeof(startup);
    command_line = windows_build_command_line(command);

    if (!CreateProcessW(NULL,
                        command_line,
                        NULL,
                        NULL,
                        TRUE,
                        0,
                        NULL,
                        NULL,
                        &startup,
                        &process)) {
        free(command_line);
        fail_message("could not start locked build command: %s", command[0]);
    }
    free(command_line);

    wait_result = WaitForSingleObject(process.hProcess, INFINITE);
    if (wait_result != WAIT_OBJECT_0 ||
        !GetExitCodeProcess(process.hProcess, &exit_code)) {
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        fail_message("could not wait for locked build command: %s", command[0]);
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return (int)exit_code;
}
#endif

static int run_build_locked(const char *root, char *const command[]) {
    BuildRootLock locked;

    if (command == NULL || command[0] == NULL) {
        fail_message("run-build requires a command");
    }

    lock_build_root(root, &locked);
    test_pause("after-build-lock");
    require_locked_root_unchanged(root, &locked);

#if defined(_WIN32)
    {
        int child_status;

        /* The launcher suppresses MSYS rewriting while entering this native helper.
         * Restore normal conversion for the actual nested MSYS build command. */
        if (_putenv_s("MSYS2_ARG_CONV_EXCL", "") != 0) {
            system_lock_release(&locked.lock);
            fail_message("could not restore MSYS argument conversion for locked build command");
        }
        child_status = windows_run_command(command);

        require_locked_root_unchanged(root, &locked);
        system_lock_release(&locked.lock);
        return child_status;
    }
#else
    {
        int wait_status;
        pid_t child;

        child = fork();
        if (child < 0) {
            system_lock_release(&locked.lock);
            fail_message("could not start locked build command: %s", command[0]);
        }
        if (child == 0) {
            execvp(command[0], command);
            fprintf(stderr,
                    "path ops: execute locked build command '%s': %s\n",
                    command[0],
                    strerror(errno));
            _exit(127);
        }

        while (waitpid(child, &wait_status, 0) < 0) {
            if (errno != EINTR) {
                system_lock_release(&locked.lock);
                fail_message("could not wait for locked build command: %s", command[0]);
            }
        }

        require_locked_root_unchanged(root, &locked);
        system_lock_release(&locked.lock);

        if (WIFEXITED(wait_status)) {
            return WEXITSTATUS(wait_status);
        }
        if (WIFSIGNALED(wait_status)) {
            return 128 + WTERMSIG(wait_status);
        }
        return 1;
    }
#endif
}

static void clean_build_root(const char *root) {
    BuildRootLock locked;
    CupError err;

    lock_build_root(root, &locked);
    test_pause("after-clean-lock");
    require_locked_root_unchanged(root, &locked);

    err = system_remove_tree_contents(root, CUP_BUILD_ROOT_MARKER, NULL);
    if (err != CUP_OK) {
        system_lock_release(&locked.lock);
        fail_system("clean build root", root, err);
    }
    require_locked_root_unchanged(root, &locked);

    err = system_remove_file_if_identity(locked.marker, &locked.marker_identity);
    system_lock_release(&locked.lock);
    if (err == CUP_OK) {
        err = system_remove_directory(root);
    }
    if (err != CUP_OK) {
        fail_system("remove cleaned build root", root, err);
    }
}

static FileCommitPolicy parse_policy(int argc, char **argv, int index) {
    if (argc <= index) {
        return FILE_COMMIT_REPLACE;
    }
    if (strcmp(argv[index], "if-different") == 0) {
        return FILE_COMMIT_IF_DIFFERENT;
    }
    if (strcmp(argv[index], "no-replace") == 0) {
        return FILE_COMMIT_NO_REPLACE;
    }
    fail_message("unsupported file commit policy: %s", argv[index]);
}

static _Noreturn void usage(void) {
    fputs("usage: path-ops COMMAND ARGUMENTS...\n", stderr);
    exit(2);
}

int main(int argc, char **argv) {
    const char *command;
    if (argc < 2) {
        usage();
    }
    command = argv[1];

    /* Protocol and validation commands. */
    if (strcmp(command, "protocol") == 0 && argc == 2) {
        printf("%d\n", CUP_PATH_OPS_PROTOCOL);
        return 0;
    }
    if (strcmp(command, "check-dir") == 0 && argc == 3) {
        check_directory(argv[2], 0);
        return 0;
    }
    if (strcmp(command, "check-dir") == 0 && argc == 4 &&
        strcmp(argv[3], "allow-missing") == 0) {
        check_directory(argv[2], 1);
        return 0;
    }

    /* Directory creation and removal. */
    if (strcmp(command, "ensure-dir") == 0 && argc == 3) {
        ensure_directory(argv[2]);
        return 0;
    }
    if (strcmp(command, "mkdir-exclusive") == 0 && argc == 3) {
        create_directory_exclusive(argv[2]);
        return 0;
    }
    if (strcmp(command, "mkdir-unique") == 0 && argc == 4) {
        create_unique_directory(argv[2], parse_mode(argv[3]));
        return 0;
    }

    /* File and tree validation. */
    if (strcmp(command, "check-file") == 0 && argc == 3) {
        check_regular_file(argv[2]);
        return 0;
    }
    if (strcmp(command, "check-tree") == 0 && argc == 3) {
        check_tree(argv[2]);
        return 0;
    }
    if (strcmp(command, "remove-tree") == 0 && argc == 3) {
        remove_tree(argv[2]);
        return 0;
    }
    if (strcmp(command, "rmdir") == 0 && argc == 3) {
        remove_empty_directory(argv[2]);
        return 0;
    }
    if (strcmp(command, "remove-file") == 0 && argc == 3) {
        remove_file(argv[2]);
        return 0;
    }

    /* Atomic publication and movement. */
    if (strcmp(command, "copy-file") == 0 && (argc == 5 || argc == 6)) {
        copy_file(argv[2],
                  argv[3],
                  parse_mode(argv[4]),
                  parse_policy(argc, argv, 5));
        return 0;
    }
    if (strcmp(command, "copy-tree") == 0 && argc == 4) {
        copy_tree(argv[2], argv[3]);
        return 0;
    }
    if (strcmp(command, "write-stdin") == 0 && (argc == 4 || argc == 5)) {
        write_stdin(argv[2],
                    parse_mode(argv[3]),
                    parse_policy(argc, argv, 4));
        return 0;
    }
    if (strcmp(command, "move") == 0 && argc == 4) {
        move_entry(argv[2], argv[3]);
        return 0;
    }

    /* Build-root ownership and locked execution. */
    if (strcmp(command, "check-build-root") == 0 && argc == 3) {
        check_build_root(argv[2]);
        return 0;
    }
    if (strcmp(command, "prepare-build-root") == 0 && argc == 3) {
        prepare_build_root(argv[2]);
        return 0;
    }
    if (strcmp(command, "run-build") == 0 && argc >= 5 && strcmp(argv[3], "--") == 0) {
        return run_build_locked(argv[2], &argv[4]);
    }
    if (strcmp(command, "clean-build-root") == 0 && argc == 3) {
        clean_build_root(argv[2]);
        return 0;
    }

    usage();
}
