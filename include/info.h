#ifndef CUP_INFO_H
#define CUP_INFO_H

#include <stddef.h>

#include "constants.h"
#include "error.h"

/* One key/value field loaded from info.txt. */
typedef struct {
    char key[MAX_INFO_KEY_LEN];
    char value[MAX_INFO_VALUE_LEN];
} PackageInfoField;

/* Dynamically sized representation of package metadata. */
typedef struct {
    PackageInfoField *fields;
    size_t count;
    size_t capacity;
} PackageInfo;

/* Initialize or release a PackageInfo object. */
void info_init(PackageInfo *info);
void info_free(PackageInfo *info);

/* Load and validate a complete info.txt file. */
CupError info_load(PackageInfo *info, const char *path);

/* Read one value or iterate over fields sharing a prefix. */
const char *info_get(const PackageInfo *info, const char *key);
const PackageInfoField *info_next(const PackageInfo *info, const char *prefix, size_t *cursor);

#endif /* CUP_INFO_H */
