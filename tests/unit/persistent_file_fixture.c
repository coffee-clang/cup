/* Test-only snapshot implementation for parser suites with mocked system APIs. */
#include "constants.h"
#include "filesystem.h"
#include "test_platform.h"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

void filesystem_snapshot_init(PersistentFileSnapshot *snapshot) {
    if (snapshot == NULL) {
        return;
    }
    memset(snapshot, 0, sizeof(*snapshot));
}

void filesystem_snapshot_release(PersistentFileSnapshot *snapshot) {
    if (snapshot == NULL) {
        return;
    }
    free(snapshot->data);
    memset(snapshot, 0, sizeof(*snapshot));
}

CupError filesystem_snapshot_read(const char *path,
                                  size_t maximum_bytes,
                                  PersistentFileSnapshot *snapshot,
                                  int *missing) {
    FILE *file = NULL;
    SystemPathIdentity identity;
    uint64_t reported_size = 0;
    unsigned char *data = NULL;
    size_t size;
    size_t read_count;
    CupError err;
    int opened_missing = 0;
    int trailing;

    if (path == NULL || path[0] == '\0' || maximum_bytes == 0 || snapshot == NULL ||
        missing == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    filesystem_snapshot_release(snapshot);
    *missing = 0;
    memset(&identity, 0, sizeof(identity));

    err = system_open_regular_file(
        path, &file, &identity, &reported_size, &opened_missing);
    if (err != CUP_OK || opened_missing) {
        *missing = opened_missing;
        return err;
    }
    if (reported_size > maximum_bytes || reported_size > SIZE_MAX - 1u) {
        return fclose(file) == 0 ? CUP_ERR_BUFFER_TOO_SMALL : CUP_ERR_FILESYSTEM;
    }
    size = (size_t)reported_size;
    data = (unsigned char *)malloc(size + 1u);
    if (data == NULL) {
        return fclose(file) == 0 ? CUP_ERR_TEMPORARY : CUP_ERR_FILESYSTEM;
    }

    read_count = size == 0 ? 0 : fread(data, 1, size, file);
    trailing = fgetc(file);
    err = read_count == size && trailing == EOF && ferror(file) == 0
              ? CUP_OK
              : CUP_ERR_FILESYSTEM;
    if (fclose(file) != 0) {
        err = CUP_ERR_FILESYSTEM;
    }
    if (err != CUP_OK) {
        free(data);
        return err;
    }
    data[size] = '\0';
    snapshot->data = data;
    snapshot->size = size;
    snapshot->identity = identity;
    return CUP_OK;
}

#if !defined(CUP_PERSISTENT_FIXTURE_NATIVE_SYSTEM)
CupError system_open_regular_file(const char *path,
                                  FILE **file,
                                  SystemPathIdentity *identity,
                                  uint64_t *file_size,
                                  int *missing) {
    TestPlatformStat status;

    if (path == NULL || file == NULL || identity == NULL || file_size == NULL ||
        missing == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    *file = NULL;
    *file_size = 0;
    *missing = 0;
    memset(identity, 0, sizeof(*identity));
    /* Preserve the CUP path bound before host CRT path handling can reinterpret it. */
    if (strlen(path) >= MAX_PATH_LEN) {
        return CUP_ERR_BUFFER_TOO_SMALL;
    }
    if (test_stat_path(path, &status) != 0) {
        if (errno == ENOENT) {
            *missing = 1;
            return CUP_OK;
        }
        return CUP_ERR_FILESYSTEM;
    }
    if (!test_stat_is_regular(&status)) {
        return CUP_ERR_FILESYSTEM;
    }
    *file = fopen(path, "rb");
    if (*file == NULL) {
        return CUP_ERR_FILESYSTEM;
    }
#if defined(_WIN32)
    if (_fstat64(_fileno(*file), &status) != 0 || !test_stat_is_regular(&status) ||
        status.st_size < 0) {
#else
    if (fstat(fileno(*file), &status) != 0 || !test_stat_is_regular(&status) ||
        status.st_size < 0) {
#endif
        fclose(*file);
        *file = NULL;
        return CUP_ERR_FILESYSTEM;
    }
    *file_size = (uint64_t)status.st_size;
    identity->volume = (uint64_t)status.st_dev;
    identity->object = (uint64_t)status.st_ino;
    identity->object_high = 0;
    identity->kind = SYSTEM_PATH_REGULAR_FILE;
    identity->valid = 1;
    return CUP_OK;
}

int system_path_identity_equal(const SystemPathIdentity *left,
                               const SystemPathIdentity *right) {
    return left != NULL && right != NULL && left->valid && right->valid &&
           left->volume == right->volume && left->object == right->object &&
           left->object_high == right->object_high && left->kind == right->kind;
}
#endif /* !CUP_PERSISTENT_FIXTURE_NATIVE_SYSTEM */
