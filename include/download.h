#ifndef CUP_DOWNLOAD_H
#define CUP_DOWNLOAD_H

/*
 * Downloads one bounded HTTPS asset into a sibling temporary file and replaces the
 * destination only after transfer and content validation succeed.
 */

#include "error.h"

typedef enum {
    DOWNLOAD_VALIDATE_NONEMPTY,
    DOWNLOAD_VALIDATE_METADATA,
    DOWNLOAD_VALIDATE_BINARY,
    DOWNLOAD_VALIDATE_ARCHIVE
} DownloadValidation;

/* Transfer, validate and atomically replace one destination file. */
CupError download_file(const char *url, const char *destination, DownloadValidation validation);

#endif /* CUP_DOWNLOAD_H */
