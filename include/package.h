#ifndef CUP_PACKAGE_H
#define CUP_PACKAGE_H

#include <stddef.h>

#include "constants.h"
#include "error.h"

#define MAX_SCANNED_PACKAGES 256
#define MAX_PACKAGE_SCAN_ISSUES 256

/* Complete identity of one concrete package. */
typedef struct {
    char component[MAX_NAME_LEN];
    char tool[MAX_NAME_LEN];
    char host_platform[MAX_PLATFORM_LEN];
    char target_platform[MAX_PLATFORM_LEN];
    char version[MAX_NAME_LEN];
} PackageIdentity;

/* Reason why an item under components cannot be treated as a valid package. */
typedef enum {
    PACKAGE_ISSUE_INVALID_PATH_TYPE,
    PACKAGE_ISSUE_INVALID_COMPONENT,
    PACKAGE_ISSUE_INVALID_TOOL,
    PACKAGE_ISSUE_INVALID_HOST,
    PACKAGE_ISSUE_INVALID_TARGET,
    PACKAGE_ISSUE_INVALID_VERSION,
    PACKAGE_ISSUE_INVALID_CONTENT
} PackageIssueReason;

/* One invalid path found while scanning the package layout. */
typedef struct {
    char path[MAX_PATH_LEN];
    PackageIssueReason reason;
    int can_quarantine;
    PackageIdentity package;
} PackageIssue;

/* Valid packages and invalid paths found under components. */
typedef struct {
    PackageIdentity items[MAX_SCANNED_PACKAGES];
    size_t count;
    size_t total_count;

    PackageIssue issues[MAX_PACKAGE_SCAN_ISSUES];
    size_t issue_count;
    size_t total_issue_count;

    int complete;
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

/* Check whether the canonical package path exists, regardless of its type. */
CupError package_path_exists(const PackageIdentity *identity, int *exists);

/* Scan and validate all package directories under components. */
CupError package_scan(PackageList *packages);
int package_list_contains(const PackageList *packages, const PackageIdentity *package);
const char *package_issue_reason_name(PackageIssueReason reason);
CupError package_quarantine(const PackageIssue *issue,
    char *recovery_path, size_t recovery_size);

#endif /* CUP_PACKAGE_H */
