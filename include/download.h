#ifndef CUP_DOWNLOAD_H
#define CUP_DOWNLOAD_H

/*
 * Downloads one bounded asset into a sibling temporary file and replaces the destination only
 * after transfer and content validation succeed. Production transfers are HTTPS-only; release
 * tests may explicitly allow HTTP for a loopback server.
 */

#include "error.h"

typedef enum {
    DOWNLOAD_VALIDATE_NONEMPTY,
    DOWNLOAD_VALIDATE_METADATA,
    DOWNLOAD_VALIDATE_BINARY,
    DOWNLOAD_VALIDATE_ARCHIVE
} DownloadValidation;

/* True only for an explicitly enabled HTTP loopback URL used by release-candidate tests. */
int download_insecure_loopback_is_allowed(const char *url);

/* Transfer, validate and atomically replace one destination file. */
CupError download_file(const char *url, const char *destination, DownloadValidation validation);

#endif /* CUP_DOWNLOAD_H */
