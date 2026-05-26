#ifndef CUP_INFO_H
#define CUP_INFO_H

#include <stddef.h>

#include "constants.h"
#include "error.h"

typedef struct {
    char key[MAX_INFO_KEY_LEN];
    char value[MAX_INFO_VALUE_LEN];
} PackageInfoField;

typedef struct {
    PackageInfoField fields[MAX_INFO_FIELDS];
    size_t count;
} PackageInfo;

int info_key_has_prefix(const char *key, const char *prefix);
CupError info_load(PackageInfo *info, const char *path);
const char *get_info_value(const PackageInfo *info, const char *key);
const PackageInfoField *next_info_field(const PackageInfo *info, const char *prefix, size_t *cursor);
size_t count_info_fields(const PackageInfo *info, const char *prefix);

#endif /* CUP_INFO_H */