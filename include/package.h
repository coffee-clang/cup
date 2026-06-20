#ifndef CUP_PACKAGE_H
#define CUP_PACKAGE_H

#include <stddef.h>

#include "constants.h"
#include "error.h"

#define MAX_SCANNED_PACKAGES 256

/* Complete identity of one concrete package. */
typedef struct {
    char component[MAX_NAME_LEN];
    char tool[MAX_NAME_LEN];
    char host_platform[MAX_PLATFORM_LEN];
    char target_platform[MAX_PLATFORM_LEN];
    char version[MAX_NAME_LEN];
} PackageIdentity;

/* Valid and invalid packages found under the components directory. */
typedef struct {
    PackageIdentity items[MAX_SCANNED_PACKAGES];
    size_t count;
    size_t invalid_count;
} PackageList;

/* Validate and initialize a package identity. */
CupError package_identity_init(PackageIdentity *identity, const char *component, const char *tool,
    const char *host_platform, const char *target_platform, const char *version);

/* Build a package identity from a concrete '<tool>@<version>' entry. */
CupError package_identity_from_entry(PackageIdentity *identity, const char *component,
    const char *host_platform, const char *target_platform, const char *entry);

/* Validate a package directory and its info.txt metadata. */
CupError package_validate(const char *base_path, const PackageIdentity *identity);

/* Check or restore the read-only protection of info.txt. */
CupError package_info_is_read_only(const char *base_path, int *is_read_only);
CupError package_set_info_read_only(const char *base_path);

/* Check whether the canonical package directory exists. */
CupError package_exists(const PackageIdentity *identity, int *exists);

/* Scan and validate all package directories under components. */
CupError package_scan(PackageList *packages);

#endif /* CUP_PACKAGE_H */
