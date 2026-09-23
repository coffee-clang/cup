/* Runs the verified native helper used to complete a cup generation update after parent exit. */

#include "update_helper.h"

#include "checksum.h"
#include "layout.h"
#include "runtime_journal.h"
#include "system.h"
#include "text.h"
#include "update_journal.h"

#include <stdio.h>
#include <string.h>

static CupError helper_matches_binary(const char *binary, const char *helper, int *matches) {
    char binary_hash[CHECKSUM_SHA256_HEX_LENGTH + 1];
    char helper_hash[CHECKSUM_SHA256_HEX_LENGTH + 1];
    SystemPathKind helper_kind;
    CupError err;

    if (matches == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    *matches = 0;

    err = system_get_path_kind(helper, &helper_kind);
    if (err != CUP_OK || helper_kind == SYSTEM_PATH_MISSING) {
        return err;
    }
    if (helper_kind != SYSTEM_PATH_REGULAR_FILE) {
        return CUP_ERR_VALIDATION;
    }
    err = checksum_sha256_file(binary, binary_hash, sizeof(binary_hash));
    if (err != CUP_OK) {
        return err;
    }
    err = checksum_sha256_file(helper, helper_hash, sizeof(helper_hash));
    if (err != CUP_OK) {
        return err;
    }

    *matches = strcmp(binary_hash, helper_hash) == 0;
    return CUP_OK;
}

CupError update_helper_prepare_from(const char *source_binary) {
    char helper[MAX_PATH_LEN];
    CupError err;
    int matches = 0;

    if (text_is_empty(source_binary)) {
        return CUP_ERR_INVALID_INPUT;
    }
    err = layout_ensure_helpers();
    if (err == CUP_OK) {
        err = layout_get_update_helper_path(helper, sizeof(helper));
    }
    if (err == CUP_OK) {
        err = helper_matches_binary(source_binary, helper, &matches);
    }
    if (err == CUP_OK && !matches) {
        err = system_copy_file(source_binary, helper);
        if (err == CUP_ERR_COMMIT) {
            /* The helper is disposable. Visibility plus byte-for-byte validation is sufficient;
             * its publication is not a canonical managed-root commit boundary. */
            err = CUP_OK;
        }
    }
    if (err == CUP_OK) {
        err = system_set_executable(helper, 1);
    }
    if (err == CUP_OK) {
        err = helper_matches_binary(source_binary, helper, &matches);
    }
    if (err == CUP_OK && !matches) {
        err = CUP_ERR_VALIDATION;
    }
    if (err != CUP_OK) {
        fprintf(stderr, "Error: could not prepare the native update helper.\n");
    }
    return err;
}

CupError update_helper_start(const char *root, const char *token, SystemLock *lock) {
    char helper[MAX_PATH_LEN];

    if (text_is_empty(root) || text_is_empty(token) || lock == NULL ||
        layout_get_update_helper_path(helper, sizeof(helper)) != CUP_OK) {
        return CUP_ERR_INVALID_INPUT;
    }
    return system_start_update_helper(helper, root, token, lock);
}

/* Detached helper execution. Handoff authority remains continuous while the parent exits and this
 * helper returns to the canonical lock before it touches update state. */
static int token_matches_temporary_name(const char *token, const char *temporary_name) {
    size_t token_length;
    size_t name_length;
    if (!runtime_journal_token_is_valid(token) || text_is_empty(temporary_name)) return 0;
    token_length = strlen(token);
    name_length = strlen(temporary_name);
    return token_length > name_length && token[token_length - name_length - 1u] == '-' &&
           strcmp(token + token_length - name_length, temporary_name) == 0;
}

/* Detached helper execution. Handoff authority remains continuous while the parent exits and this
 * helper returns to the canonical lock before touching generation state. */
CupError update_helper_run(const char *root,
                           const char *token,
                           const char *parent_signal_value,
                           const char *authority_value) {
    UpdateJournal journal;
    UpdateJournalStatus status;
    SystemHandoff handoff = {0};
    SystemLock lock = {0};
    char selected_root[MAX_PATH_LEN];
    char lock_path[MAX_PATH_LEN];
    CupError err;

    if (text_is_empty(root) || text_is_empty(token)) return CUP_ERR_INVALID_INPUT;
    err = system_handoff_accept(&handoff, parent_signal_value, authority_value);
    if (err == CUP_OK) err = layout_build_lock_path(lock_path, sizeof(lock_path), root);
    if (err == CUP_OK) err = system_handoff_acquire_lock(&handoff, &lock, lock_path);
    if (err != CUP_OK) {
        system_handoff_release(&handoff);
        return err;
    }

    err = layout_root_snapshot_begin_at(root);
    if (err == CUP_OK) err = layout_get_root(selected_root, sizeof(selected_root));
    if (err == CUP_OK && strcmp(selected_root, root) != 0) err = CUP_ERR_TRANSACTION;
    if (err == CUP_OK) err = layout_root_snapshot_validate();
    if (err == CUP_OK) err = update_journal_load(&journal, &status);
    if (err == CUP_OK &&
        (status != CUP_UPDATE_JOURNAL_LOADED ||
         !token_matches_temporary_name(token, journal.temporary_name))) {
        err = CUP_ERR_TRANSACTION;
    }
    if (err == CUP_OK) err = update_generation_commit(&journal);

    layout_root_snapshot_end();
    system_lock_release(&lock);
    return err;
}
