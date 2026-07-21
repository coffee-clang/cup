#ifndef CUP_ERROR_H
#define CUP_ERROR_H

/* Stable internal error model shared by all modules. */

typedef enum {
    CUP_OK = 0,

    /* Input / cli. */
    CUP_ERR_INVALID_INPUT,

    /* Supported domain. */
    CUP_ERR_UNSUPPORTED_COMPONENT,
    CUP_ERR_INVALID_TOOL,
    CUP_ERR_INVALID_RELEASE,
    CUP_ERR_INVALID_OS,
    CUP_ERR_INVALID_ARCH,
    CUP_ERR_NOT_AVAILABLE,

    /* PackageCatalog. */
    CUP_ERR_CATALOG,

    /* State. */
    CUP_ERR_STATE_LOAD,
    CUP_ERR_STATE_SAVE,
    CUP_ERR_STATE_FULL,
    CUP_ERR_ACTIVE_FULL,
    CUP_ERR_ALREADY_INSTALLED,
    CUP_ERR_NOT_INSTALLED,
    CUP_ERR_INCONSISTENT_STATE,

    /* Filesystem / temporary storage. */
    CUP_ERR_FILESYSTEM,
    CUP_ERR_TEMPORARY,
    CUP_ERR_LOCK,
    CUP_ERR_TRANSACTION,

    /* Buffer / size limits. */
    CUP_ERR_BUFFER_TOO_SMALL,

    /* Package transfer / archives. */
    CUP_ERR_FETCH,
    CUP_ERR_TLS,
    CUP_ERR_TIMEOUT,
    CUP_ERR_DOWNLOAD_TOO_LARGE,
    CUP_ERR_ARCHIVE,
    CUP_ERR_ARCHIVE_UNSAFE,
    CUP_ERR_EXTRACT,

    /*
     * Reserved to keep the historical numeric exit code stable even though
     * SHA-256 no longer uses an external cryptographic provider.
     */
    CUP_ERR_CRYPTO,

    /* Install validation / transactions. */
    CUP_ERR_VALIDATION,
    CUP_ERR_COMMIT,
    CUP_ERR_ROLLBACK,

    /* Interrupt. */
    CUP_ERR_INTERRUPT
} CupError;

#endif /* CUP_ERROR_H */
