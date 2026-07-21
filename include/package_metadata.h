#ifndef CUP_PACKAGE_METADATA_H
#define CUP_PACKAGE_METADATA_H

/*
 * Module contract: Generic immutable info.txt key/value storage and ordered
 * field iteration. Package identity policy is enforced by package.c.
 */

#include <stddef.h>

#include "constants.h"
#include "error.h"

/* One validated key/value field loaded from info.txt. */
typedef struct {
    char key[MAX_METADATA_KEY_LEN];
    char value[MAX_METADATA_VALUE_LEN];
} PackageMetadataField;

/* Dynamically sized metadata collection owned by the caller. */
typedef struct {
    PackageMetadataField *fields;
    size_t count;
    size_t capacity;
} PackageMetadata;

/* One package command decoded from the external entry.<name> boundary. */
typedef struct {
    char name[MAX_COMMAND_NAME_LEN];
    char path[MAX_METADATA_VALUE_LEN];
} PackageCommand;

/* Initialize or release all storage owned by a PackageMetadata. */
void package_metadata_init(PackageMetadata *metadata);
void package_metadata_free(PackageMetadata *metadata);

/* Load the complete file, rejecting malformed, duplicate, or unsafe fields. */
CupError package_metadata_load(PackageMetadata *metadata, const char *path);

/* Return one exact value, or NULL when the key is absent. */
const char *package_metadata_get(const PackageMetadata *metadata, const char *key);

/*
 * Return the next field sharing prefix and advance the caller-owned cursor.
 * File order is preserved.
 */
const PackageMetadataField *package_metadata_next(const PackageMetadata *metadata,
                                                  const char *prefix,
                                                  size_t *cursor);

/* Decode the next package command while keeping entry.* at this boundary. */
int package_metadata_next_command(const PackageMetadata *metadata,
                                  PackageCommand *command,
                                  size_t *cursor);

#endif /* CUP_PACKAGE_METADATA_H */
