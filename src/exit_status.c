/*
 * Maps internal errors to stable public exit statuses.
 */

#include "exit_status.h"

/* Keep this mapping explicit so internal enum changes cannot alter the public process contract. */
int cup_error_to_exit_status(CupError error) {
    switch (error) {
        case CUP_OK:
        case CUP_ERR_ALREADY_INSTALLED:
            return CUP_STATUS_SUCCESS;

        case CUP_ERR_INVALID_INPUT:
        case CUP_ERR_UNSUPPORTED_COMPONENT:
        case CUP_ERR_INVALID_TOOL:
        case CUP_ERR_INVALID_RELEASE:
        case CUP_ERR_INVALID_OS:
        case CUP_ERR_INVALID_ARCH:
            return CUP_STATUS_USAGE;

        case CUP_ERR_NOT_AVAILABLE:
        case CUP_ERR_NOT_INSTALLED:
            return CUP_STATUS_UNAVAILABLE;

        case CUP_ERR_CATALOG:
        case CUP_ERR_STATE_LOAD:
        case CUP_ERR_STATE_SAVE:
        case CUP_ERR_STATE_FULL:
        case CUP_ERR_ACTIVE_FULL:
        case CUP_ERR_INCONSISTENT_STATE:
        case CUP_ERR_VALIDATION:
            return CUP_STATUS_STATE;

        case CUP_ERR_FETCH:
        case CUP_ERR_TLS:
        case CUP_ERR_TIMEOUT:
        case CUP_ERR_DOWNLOAD_TOO_LARGE:
            return CUP_STATUS_NETWORK;

        case CUP_ERR_FILESYSTEM:
        case CUP_ERR_TEMPORARY:
        case CUP_ERR_LOCK:
        case CUP_ERR_TRANSACTION:
        case CUP_ERR_ARCHIVE:
        case CUP_ERR_ARCHIVE_UNSAFE:
        case CUP_ERR_EXTRACT:
        case CUP_ERR_COMMIT:
        case CUP_ERR_ROLLBACK:
            return CUP_STATUS_OPERATION;

        case CUP_ERR_INTERRUPT:
            return CUP_STATUS_INTERRUPT;

        case CUP_ERR_BUFFER_TOO_SMALL:
        case CUP_ERR_CRYPTO:
        default:
            return CUP_STATUS_INTERNAL;
    }
}
