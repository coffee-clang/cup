#ifndef CUP_SYSTEM_H
#define CUP_SYSTEM_H

#include <stddef.h>

#include "error.h"

typedef struct {
    int is_directory;
    int is_reparse_point;
    int is_regular_file;
} SystemPathInfo;

typedef CupError (*SystemDirectoryCallback)(const char *path, const SystemPathInfo *info, void *userdata);

CupError system_get_home_dir(char *buffer, size_t size);
CupError system_get_process_id(char *buffer, size_t size);
CupError system_start_uninstall(const char *cup_root, const char *uninstall_script);

CupError system_make_directory(const char *path);
CupError system_remove_directory(const char *path);
CupError system_rename_path(const char *source, const char *destination);
CupError system_remove_file(const char *path);

CupError system_path_exists(const char *path, int *exists);
CupError system_is_directory(const char *path, int *is_directory);
CupError system_is_regular_file(const char *path, int *is_regular_file);
CupError system_file_size(const char *path, long long *file_size);

CupError system_walk_directory(const char *path, SystemDirectoryCallback callback, void *userdata);

#endif /* CUP_SYSTEM_H */