#ifndef CUP_PACKAGE_H
#define CUP_PACKAGE_H

/*
 * Concrete package identities, installed-package validation, component-tree scanning, and
 * quarantine of invalid paths.
 */

#include <stddef.h>

#include "constants.h"
#include "error.h"

#define MAX_SCANNED_PACKAGES 256
#define MAX_PACKAGE_SCAN_ISSUES 256

/* Component, host and target shared by package selections and identities. */
typedef struct {
    char component[MAX_IDENTIFIER_LEN];
    char host_platform[MAX_PLATFORM_LEN];
    char target_platform[MAX_PLATFORM_LEN];
} PackageScope;

/* Complete identity of one concrete package. */
typedef struct {
    char component[MAX_IDENTIFIER_LEN];
    char tool[MAX_IDENTIFIER_LEN];
    char host_platform[MAX_PLATFORM_LEN];
    char target_platform[MAX_PLATFORM_LEN];
    char version[MAX_IDENTIFIER_LEN];
} PackageIdentity;

/* Reason why an item under components cannot be accepted as a package. */
typedef enum {
    PACKAGE_ISSUE_INVALID_PATH_TYPE,
    PACKAGE_ISSUE_INVALID_COMPONENT,
    PACKAGE_ISSUE_INVALID_TOOL,
    PACKAGE_ISSUE_INVALID_HOST,
    PACKAGE_ISSUE_INVALID_TARGET,
    PACKAGE_ISSUE_INVALID_VERSION,
    PACKAGE_ISSUE_INVALID_CONTENT
} PackageIssueReason;

/* One invalid path discovered while scanning the package layout. */
typedef struct {
    char path[MAX_PATH_LEN];
    PackageIssueReason reason;
    int can_quarantine;
    PackageIdentity package;
} PackageIssue;

/*
 * Bounded scan result. total_* counters retain the real discovered totals;
 * complete is false when a bounded array could not retain every item.
 */
typedef struct {
    PackageIdentity items[MAX_SCANNED_PACKAGES];
    size_t count;
    size_t total_count;

    PackageIssue issues[MAX_PACKAGE_SCAN_ISSUES];
    size_t issue_count;
    size_t total_issue_count;

    size_t foreign_host_count;
    int complete;
} PackageList;

/* Validate and initialize one component/host/target scope. */
CupError package_scope_init(PackageScope *scope,
                            const char *component,
                            const char *host_platform,
                            const char *target_platform);
int package_scope_equals(const PackageScope *left, const PackageScope *right);

/* Copy the scope represented by one concrete identity. */
CupError package_identity_get_scope(const PackageIdentity *identity, PackageScope *scope);

/* Compare complete concrete identities. */
int package_identity_equals(const PackageIdentity *left, const PackageIdentity *right);

/* Validate an already initialized concrete identity. */
CupError package_identity_validate(const PackageIdentity *identity);

/* Format the canonical concrete '<tool>@<version>' selector. */
CupError package_identity_format_selector(const PackageIdentity *identity,
                                          char *buffer,
                                          size_t size);

/* Validate all fields and initialize one concrete package identity. */
CupError package_identity_init(PackageIdentity *identity,
                               const char *component,
                               const char *tool,
                               const char *host_platform,
                               const char *target_platform,
                               const char *version);

/* Build an identity from a canonical concrete '<tool>@<version>' selector. */
CupError package_identity_from_selector(PackageIdentity *identity,
                                        const char *component,
                                        const char *host_platform,
                                        const char *target_platform,
                                        const char *selector);

/*
 * Validate the package root, immutable info.txt identity, and every declared
 * executable selector without trusting the directory name alone.
 */
CupError package_validate(const char *base_path, const PackageIdentity *identity);

/* Inspect or restore the immutable protection applied to info.txt. */
CupError package_metadata_is_read_only(const char *base_path, int *is_read_only);
CupError package_set_metadata_read_only(const char *base_path);

/* Check whether the canonical package path exists, regardless of path type. */
CupError package_path_exists(const PackageIdentity *identity, int *exists);

/* Scan and validate all paths below the canonical components directory. */
CupError package_scan(PackageList *packages);
int package_list_contains(const PackageList *packages, const PackageIdentity *package);
const char *package_issue_reason_name(PackageIssueReason reason);

/* Move one quarantinable invalid path into a unique recovery directory. */
CupError package_quarantine(const PackageIssue *issue, char *recovery_path, size_t recovery_size);

#endif /* CUP_PACKAGE_H */
