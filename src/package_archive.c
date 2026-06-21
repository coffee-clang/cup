#include "package_archive.h"

#include "constants.h"
#include "interrupt.h"
#include "system.h"
#include "text.h"

#include <archive.h>
#include <archive_entry.h>
#include <stdint.h>

#define ARCHIVE_READ_BLOCK_SIZE 10240

CupError package_archive_open_reader(struct archive **reader,
    const char *archive_path) {
    struct archive *archive_reader;
    int status;

    if (reader == NULL || text_is_empty(archive_path)) {
        return CUP_ERR_INVALID_INPUT;
    }

    *reader = NULL;
    archive_reader = archive_read_new();
    if (archive_reader == NULL) {
        return CUP_ERR_ARCHIVE;
    }

    status = archive_read_support_filter_all(archive_reader);
    if (status == ARCHIVE_OK) {
        status = archive_read_support_format_all(archive_reader);
    }
    if (status == ARCHIVE_OK) {
        status = archive_read_open_filename(archive_reader, archive_path,
            ARCHIVE_READ_BLOCK_SIZE);
    }
    if (status != ARCHIVE_OK) {
        archive_read_free(archive_reader);
        return CUP_ERR_ARCHIVE;
    }

    *reader = archive_reader;
    return CUP_OK;
}

static int close_reader(struct archive *reader) {
    int valid = 1;

    if (archive_read_close(reader) != ARCHIVE_OK) {
        valid = 0;
    }
    if (archive_read_free(reader) != ARCHIVE_OK) {
        valid = 0;
    }

    return valid;
}

static CupError read_entry_data(struct archive *reader,
    la_int64_t declared_size, uint64_t *read_total) {
    const void *buffer;
    size_t size;
    la_int64_t offset;
    int status;

    while (1) {
        uint64_t end;

        if (interrupt_requested()) {
            return CUP_ERR_INTERRUPT;
        }

        status = archive_read_data_block(reader, &buffer, &size, &offset);
        if (status == ARCHIVE_EOF) {
            return CUP_OK;
        }
        if (status != ARCHIVE_OK || offset < 0 ||
            (uint64_t)offset > UINT64_MAX - size) {
            return CUP_ERR_ARCHIVE;
        }

        end = (uint64_t)offset + size;
        if ((declared_size >= 0 && end > (uint64_t)declared_size) ||
            size > MAX_PACKAGE_EXTRACTED_BYTES - *read_total) {
            return CUP_ERR_ARCHIVE_UNSAFE;
        }

        *read_total += size;
    }
}

static CupError account_declared_size(struct archive_entry *entry,
    uint64_t *declared_total) {
    la_int64_t size;

    if (archive_entry_filetype(entry) != AE_IFREG ||
        archive_entry_hardlink(entry) != NULL) {
        return CUP_OK;
    }

    size = archive_entry_size(entry);
    if (size < 0 ||
        (uint64_t)size > MAX_PACKAGE_EXTRACTED_BYTES - *declared_total) {
        return CUP_ERR_ARCHIVE_UNSAFE;
    }

    *declared_total += (uint64_t)size;
    return CUP_OK;
}

CupError package_archive_is_valid(const char *archive_path, int *is_valid) {
    struct archive *reader = NULL;
    struct archive_entry *entry;
    CupError err;
    SystemPathKind kind;
    long long file_size;
    uint64_t declared_total = 0;
    uint64_t read_total = 0;
    size_t entry_count = 0;
    int has_payload = 0;
    int status;

    if (is_valid == NULL || text_is_empty(archive_path)) {
        return CUP_ERR_INVALID_INPUT;
    }
    *is_valid = 0;

    err = system_get_path_kind(archive_path, &kind);
    if (err != CUP_OK) {
        return err;
    }
    if (kind == SYSTEM_PATH_MISSING) {
        return CUP_OK;
    }
    if (kind != SYSTEM_PATH_REGULAR_FILE) {
        return CUP_OK;
    }

    err = system_file_size(archive_path, &file_size);
    if (err != CUP_OK) {
        return err;
    }
    if (file_size <= 0) {
        return CUP_OK;
    }

    err = package_archive_open_reader(&reader, archive_path);
    if (err != CUP_OK) {
        return CUP_OK;
    }

    while (1) {
        la_int64_t declared_size;

        if (interrupt_requested()) {
            archive_read_close(reader);
            archive_read_free(reader);
            return CUP_ERR_INTERRUPT;
        }

        status = archive_read_next_header(reader, &entry);
        if (status == ARCHIVE_EOF) {
            break;
        }
        if (status != ARCHIVE_OK) {
            close_reader(reader);
            return CUP_OK;
        }

        entry_count++;
        if (entry_count > MAX_PACKAGE_ARCHIVE_ENTRIES) {
            close_reader(reader);
            return CUP_OK;
        }

        if (archive_entry_filetype(entry) != AE_IFDIR) {
            has_payload = 1;
        }

        err = account_declared_size(entry, &declared_total);
        if (err != CUP_OK) {
            close_reader(reader);
            return err == CUP_ERR_ARCHIVE_UNSAFE ? CUP_OK : err;
        }

        declared_size = archive_entry_size(entry);
        err = read_entry_data(reader, declared_size, &read_total);
        if (err == CUP_ERR_INTERRUPT) {
            archive_read_close(reader);
            archive_read_free(reader);
            return err;
        }
        if (err != CUP_OK) {
            close_reader(reader);
            return CUP_OK;
        }
    }

    if (!close_reader(reader) || entry_count == 0 || !has_payload) {
        return CUP_OK;
    }

    *is_valid = 1;
    return CUP_OK;
}
