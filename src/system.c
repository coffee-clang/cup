/*
 * Platform-neutral filesystem query wrappers built on the native system backend.
 */

#include "system.h"

#include "text.h"

CupError system_path_exists(const char *path, int *exists) {
    SystemPathKind kind;
    CupError err;

    if (exists == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    *exists = 0;

    err = system_get_path_kind(path, &kind);
    if (err != CUP_OK) {
        return err;
    }
    *exists = kind != SYSTEM_PATH_MISSING;
    return CUP_OK;
}

CupError system_is_directory(const char *path, int *is_directory) {
    SystemPathKind kind;
    CupError err;

    if (is_directory == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    *is_directory = 0;

    err = system_get_path_kind(path, &kind);
    if (err != CUP_OK) {
        return err;
    }
    *is_directory = kind == SYSTEM_PATH_DIRECTORY;
    return CUP_OK;
}

CupError system_is_regular_file(const char *path, int *is_regular_file) {
    SystemPathKind kind;
    CupError err;

    if (is_regular_file == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    *is_regular_file = 0;

    err = system_get_path_kind(path, &kind);
    if (err != CUP_OK) {
        return err;
    }
    *is_regular_file = kind == SYSTEM_PATH_REGULAR_FILE;
    return CUP_OK;
}
