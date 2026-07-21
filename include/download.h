#ifndef CUP_DOWNLOAD_H
#define CUP_DOWNLOAD_H

/* Module contract: Bounded HTTPS transfer with atomic destination replacement. */

#include "error.h"

typedef enum {
    DOWNLOAD_VALIDATE_NONEMPTY,
    DOWNLOAD_VALIDATE_METADATA,
    DOWNLOAD_VALIDATE_BINARY,
    DOWNLOAD_VALIDATE_ARCHIVE
} DownloadValidation;

CupError download_file(const char *url, const char *destination, DownloadValidation validation);

#endif /* CUP_DOWNLOAD_H */
