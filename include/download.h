#ifndef CUP_DOWNLOAD_H
#define CUP_DOWNLOAD_H

/*
 * Downloads one size-bounded asset through a sibling temporary file. The destination is replaced
 * only after transfer and content validation succeed. Production uses HTTPS; release tests may
 * explicitly allow HTTP for a loopback server.
 */

#include <stddef.h>

#include "error.h"

typedef enum {
    DOWNLOAD_VALIDATE_NONEMPTY,
    DOWNLOAD_VALIDATE_METADATA,
    DOWNLOAD_VALIDATE_BINARY,
    DOWNLOAD_VALIDATE_ARCHIVE
} DownloadValidation;

/* Optional validators inspect the completed temporary file only; they must not mutate, replace or
 * remove temporary_path. */
typedef CupError (*DownloadValidator)(const char *temporary_path, void *userdata);

/* True only for an explicitly enabled HTTP loopback URL used by release-candidate tests. */
int download_insecure_loopback_is_allowed(const char *url);

/* Copy the validated optional release-source override without trailing separators.
 * A writable output is cleared before any non-success result. */
CupError download_copy_release_base_override(char *base, size_t size);

/* Transfer, validate and atomically replace one destination file. */
CupError download_file_checked(const char *url,
                               const char *destination,
                               DownloadValidation validation,
                               DownloadValidator validator,
                               void *validator_data);

/* Simpler transfer boundary for callers that do not need a validator. */
CupError download_file(const char *url, const char *destination, DownloadValidation validation);

#endif /* CUP_DOWNLOAD_H */
