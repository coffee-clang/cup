/*
 * Runs the managed native helper copy used to complete a CUP update after the parent process
 * releases the executable and lock.
 */

#include "cup_update_helper.h"

#include "constants.h"
#include "cup_update_journal.h"
#include "filesystem.h"
#include "layout.h"
#include "path.h"
#include "system.h"
#include "text.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <limits.h>
#include <wchar.h>
#include <windows.h>
static HANDLE g_parent_signal = NULL;
#else
#include <fcntl.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
static int g_parent_signal = -1;
#endif

#define HELPER_LOCK_ATTEMPTS 600
#define HELPER_LOCK_DELAY_MS 100

typedef struct {
    const char *new_name;
    const char *old_name;
    char destination[MAX_PATH_LEN];
    int executable;
    int read_only;
} HelperAsset;

/* Fixed generation asset table. The helper replaces only the official CUP assets described here. */
static CupError build_asset_path(const char *staging, const char *name, char *buffer, size_t size) {
    return path_join(buffer, size, staging, name);
}

static CupError initialize_assets(HelperAsset *assets, size_t count) {
    if (assets == NULL || count != 6) {
        return CUP_ERR_INVALID_INPUT;
    }

    memset(assets, 0, sizeof(*assets) * count);
    assets[0].new_name = CUP_UPDATE_BINARY_NEW;
    assets[0].old_name = CUP_UPDATE_BINARY_OLD;
    assets[0].executable = 1;
    if (layout_get_binary_path(assets[0].destination, sizeof(assets[0].destination)) != CUP_OK)
        return CUP_ERR_TRANSACTION;

    assets[1].new_name = CUP_UPDATE_UNINSTALL_NEW;
    assets[1].old_name = CUP_UPDATE_UNINSTALL_OLD;
    assets[1].executable = 1;
    assets[1].read_only = 1;
    if (layout_get_uninstall_path(assets[1].destination, sizeof(assets[1].destination)) != CUP_OK)
        return CUP_ERR_TRANSACTION;

    assets[2].new_name = CUP_UPDATE_PLATFORM_CHECKSUMS_NEW;
    assets[2].old_name = CUP_UPDATE_PLATFORM_CHECKSUMS_OLD;
    assets[2].read_only = 1;
    if (layout_get_platform_checksums_path(assets[2].destination, sizeof(assets[2].destination)) !=
        CUP_OK)
        return CUP_ERR_TRANSACTION;

    assets[3].new_name = CUP_UPDATE_PACKAGES_NEW;
    assets[3].old_name = CUP_UPDATE_PACKAGES_OLD;
    assets[3].read_only = 1;
    if (layout_get_package_catalog_path(assets[3].destination, sizeof(assets[3].destination)) !=
        CUP_OK)
        return CUP_ERR_TRANSACTION;

    assets[4].new_name = CUP_UPDATE_INSTALL_POLICY_NEW;
    assets[4].old_name = CUP_UPDATE_INSTALL_POLICY_OLD;
    assets[4].read_only = 1;
    if (layout_get_install_policy_path(assets[4].destination, sizeof(assets[4].destination)) !=
        CUP_OK)
        return CUP_ERR_TRANSACTION;

    assets[5].new_name = CUP_UPDATE_COMMON_CHECKSUMS_NEW;
    assets[5].old_name = CUP_UPDATE_COMMON_CHECKSUMS_OLD;
    assets[5].read_only = 1;
    if (layout_get_common_checksums_path(assets[5].destination, sizeof(assets[5].destination)) !=
        CUP_OK)
        return CUP_ERR_TRANSACTION;

    return CUP_OK;
}

static CupError set_asset_permissions(const HelperAsset *asset) {
    CupError err;

    if (asset->executable) {
        err = system_set_executable(asset->destination, 1);
        if (err != CUP_OK)
            return err;
    }
    if (asset->read_only) {
        err = system_set_read_only(asset->destination, 1);
        if (err != CUP_OK)
            return err;
    }
    return CUP_OK;
}

static CupError create_empty_marker(const char *path) {
    FILE *file = NULL;
    CupError err = system_create_file_exclusive(path, &file);
    int sync_failed;
    int close_failed;

    if (err != CUP_OK)
        return err;
    sync_failed = system_sync_file(file) != CUP_OK;
    close_failed = fclose(file) != 0;
    if (sync_failed || close_failed || system_sync_parent_directory(path) != CUP_OK) {
        return CUP_ERR_COMMIT;
    }
    return CUP_OK;
}

static CupError validate_staged_assets(const char *staging,
                                       const HelperAsset *assets,
                                       size_t count) {
    size_t i;

    for (i = 0; i < count; ++i) {
        char source[MAX_PATH_LEN];
        SystemPathKind kind;

        if (build_asset_path(staging, assets[i].new_name, source, sizeof(source)) != CUP_OK ||
            system_get_path_kind(source, &kind) != CUP_OK || kind != SYSTEM_PATH_REGULAR_FILE) {
            return CUP_ERR_VALIDATION;
        }
    }
    return CUP_OK;
}

/* Commit protocol. Destinations are backed up before replacement and restored only while the commit
 * marker is absent. */
static CupError backup_destinations(const char *staging, const HelperAsset *assets, size_t count) {
    size_t i;

    for (i = 0; i < count; ++i) {
        char backup[MAX_PATH_LEN];
        SystemPathKind kind;
        SystemCommitState commit_state = SYSTEM_COMMIT_NOT_APPLIED;
        CupError err;

        if (build_asset_path(staging, assets[i].old_name, backup, sizeof(backup)) != CUP_OK ||
            system_get_path_kind(assets[i].destination, &kind) != CUP_OK) {
            return CUP_ERR_TRANSACTION;
        }
        if (kind == SYSTEM_PATH_MISSING) {
            return CUP_ERR_VALIDATION;
        }
        if (kind != SYSTEM_PATH_REGULAR_FILE ||
            system_set_read_only(assets[i].destination, 0) != CUP_OK) {
            return CUP_ERR_TRANSACTION;
        }
        err = system_move_path(assets[i].destination, backup, &commit_state);
        if (err != CUP_OK) {
            return commit_state == SYSTEM_COMMIT_NOT_APPLIED ? CUP_ERR_TRANSACTION : CUP_ERR_COMMIT;
        }
    }
    return CUP_OK;
}

static CupError install_staged_assets(const char *staging,
                                      const HelperAsset *assets,
                                      size_t count) {
    size_t i;

    for (i = 0; i < count; ++i) {
        char source[MAX_PATH_LEN];
        SystemCommitState commit_state = SYSTEM_COMMIT_NOT_APPLIED;
        CupError err;

        if (build_asset_path(staging, assets[i].new_name, source, sizeof(source)) != CUP_OK) {
            return CUP_ERR_TRANSACTION;
        }
        err = system_move_path(source, assets[i].destination, &commit_state);
        if (err != CUP_OK) {
            return commit_state == SYSTEM_COMMIT_NOT_APPLIED ? CUP_ERR_TRANSACTION : CUP_ERR_COMMIT;
        }
        if (set_asset_permissions(&assets[i]) != CUP_OK) {
            return CUP_ERR_COMMIT;
        }
    }
    return CUP_OK;
}

static CupError commit_update(CupUpdateJournal *journal, const char *staging) {
    HelperAsset assets[6];
    char marker[MAX_PATH_LEN];
    CupError err;

    err = initialize_assets(assets, sizeof(assets) / sizeof(assets[0]));
    if (err == CUP_OK) {
        err = validate_staged_assets(staging, assets, sizeof(assets) / sizeof(assets[0]));
    }
    if (err == CUP_OK) {
        err = cup_update_journal_set_phase(journal, CUP_UPDATE_PHASE_COMMITTING, 0);
    }
    if (err == CUP_OK) {
        err = backup_destinations(staging, assets, sizeof(assets) / sizeof(assets[0]));
    }
    if (err == CUP_OK) {
        err = install_staged_assets(staging, assets, sizeof(assets) / sizeof(assets[0]));
    }
    if (err == CUP_OK) {
        err = build_asset_path(staging, CUP_UPDATE_COMMITTED, marker, sizeof(marker));
    }
    if (err == CUP_OK) {
        err = create_empty_marker(marker);
    }
    if (err == CUP_OK) {
        err = cup_update_journal_clear();
    }
    if (err == CUP_OK) {
        (void)cup_update_result_write(CUP_UPDATE_RESULT_SUCCESS, 0, journal->version);
        if (filesystem_remove_tree(staging) != CUP_OK) {
            return CUP_ERR_COMMIT;
        }
    }
    return err;
}

/* Persistent completion channel. Detached failures are recorded for the next command and for doctor
 * diagnostics. */
static void record_helper_failure(CupUpdateJournal *journal, CupError error, int recover) {
    if (journal == NULL)
        return;

    if (recover &&
        cup_update_journal_set_phase(journal, CUP_UPDATE_PHASE_FAILED, (int)error) == CUP_OK) {
        if (cup_update_journal_recover(journal) != CUP_OK) {
            (void)cup_update_journal_set_phase(journal, CUP_UPDATE_PHASE_FAILED, (int)error);
        }
    } else {
        (void)cup_update_journal_set_phase(journal, CUP_UPDATE_PHASE_FAILED, (int)error);
    }
    (void)cup_update_result_write(CUP_UPDATE_RESULT_FAILED, (int)error, journal->version);
}

static CupError acquire_helper_lock(SystemLock *lock) {
    char lock_path[MAX_PATH_LEN];
    int attempt;

    if (layout_get_lock_path(lock_path, sizeof(lock_path)) != CUP_OK) {
        return CUP_ERR_LOCK;
    }
    for (attempt = 0; attempt < HELPER_LOCK_ATTEMPTS; ++attempt) {
        CupError err = system_lock_acquire(lock, lock_path, SYSTEM_LOCK_EXCLUSIVE);
        if (err == CUP_OK)
            return CUP_OK;
        if (err != CUP_ERR_LOCK)
            return err;
#if defined(_WIN32)
        Sleep(HELPER_LOCK_DELAY_MS);
#else
        {
            struct timespec delay;
            delay.tv_sec = 0;
            delay.tv_nsec = HELPER_LOCK_DELAY_MS * 1000000L;
            nanosleep(&delay, NULL);
        }
#endif
    }
    return CUP_ERR_LOCK;
}

static CupError wait_for_parent(const char *value) {
    char *end;
#if defined(_WIN32)
    unsigned long long number;
    HANDLE handle;
    char byte;
    DWORD read_count;

    errno = 0;
    number = strtoull(value, &end, 10);
    if (errno != 0 || *end != '\0' || number == 0)
        return CUP_ERR_INVALID_INPUT;
    handle = (HANDLE)(uintptr_t)number;
    while (1) {
        if (!ReadFile(handle, &byte, 1, &read_count, NULL)) {
            DWORD error = GetLastError();

            CloseHandle(handle);
            return error == ERROR_BROKEN_PIPE ? CUP_OK : CUP_ERR_FILESYSTEM;
        }
        if (read_count == 0) {
            CloseHandle(handle);
            return CUP_OK;
        }
    }
#else
    long number;
    int descriptor;
    char byte;
    ssize_t count;

    errno = 0;
    number = strtol(value, &end, 10);
    if (errno != 0 || *end != '\0' || number < 0 || number > 0x7fffffffL) {
        return CUP_ERR_INVALID_INPUT;
    }
    descriptor = (int)number;
    do {
        count = read(descriptor, &byte, 1);
    } while (count > 0 || (count < 0 && errno == EINTR));
    close(descriptor);
    if (count < 0)
        return CUP_ERR_FILESYSTEM;
#endif
    return CUP_OK;
}

/* Parent-side handoff. The helper executable is copied to staging before the running CUP binary
 * releases control. */
CupError cup_update_helper_prepare(void) {
    char binary[MAX_PATH_LEN];
    char helper[MAX_PATH_LEN];
    CupError err;

    err = layout_ensure_cup_assets();
    if (err == CUP_OK)
        err = layout_get_binary_path(binary, sizeof(binary));
    if (err == CUP_OK)
        err = layout_get_cup_update_helper_path(helper, sizeof(helper));
    if (err == CUP_OK)
        err = system_copy_file(binary, helper);
    if (err == CUP_OK)
        err = system_set_executable(helper, 1);
    return err;
}

#if defined(_WIN32)
static CupError utf8_to_wide(const char *input, wchar_t *output, size_t output_count) {
    int written;
    if (text_is_empty(input) || output == NULL || output_count == 0 || output_count > INT_MAX)
        return CUP_ERR_INVALID_INPUT;
    written =
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input, -1, output, (int)output_count);
    return written == 0 ? CUP_ERR_FILESYSTEM : CUP_OK;
}
#endif

CupError cup_update_helper_start(const char *token) {
    char helper[MAX_PATH_LEN];

    if (text_is_empty(token) ||
        layout_get_cup_update_helper_path(helper, sizeof(helper)) != CUP_OK) {
        return CUP_ERR_INVALID_INPUT;
    }

#if defined(_WIN32)
    SECURITY_ATTRIBUTES security;
    HANDLE read_handle = NULL;
    HANDLE write_handle = NULL;
    wchar_t wide_helper[MAX_PATH_LEN];
    wchar_t wide_token[MAX_PATH_LEN];
    wchar_t command[MAX_PATH_LEN * 3];
    STARTUPINFOW startup;
    PROCESS_INFORMATION process;
    int written;

    memset(&security, 0, sizeof(security));
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;
    if (!CreatePipe(&read_handle, &write_handle, &security, 0) ||
        !SetHandleInformation(write_handle, HANDLE_FLAG_INHERIT, 0) ||
        utf8_to_wide(helper, wide_helper, MAX_PATH_LEN) != CUP_OK ||
        utf8_to_wide(token, wide_token, MAX_PATH_LEN) != CUP_OK) {
        if (read_handle != NULL)
            CloseHandle(read_handle);
        if (write_handle != NULL)
            CloseHandle(write_handle);
        return CUP_ERR_FILESYSTEM;
    }
    written = _snwprintf(command,
                         sizeof(command) / sizeof(command[0]),
                         L"\"%ls\" --internal-cup-update-helper %ls %llu",
                         wide_helper,
                         wide_token,
                         (unsigned long long)(uintptr_t)read_handle);
    if (written < 0 || (size_t)written >= sizeof(command) / sizeof(command[0])) {
        CloseHandle(read_handle);
        CloseHandle(write_handle);
        return CUP_ERR_BUFFER_TOO_SMALL;
    }
    ZeroMemory(&startup, sizeof(startup));
    startup.cb = sizeof(startup);
    ZeroMemory(&process, sizeof(process));
    if (!CreateProcessW(wide_helper,
                        command,
                        NULL,
                        NULL,
                        TRUE,
                        CREATE_NO_WINDOW,
                        NULL,
                        NULL,
                        &startup,
                        &process)) {
        CloseHandle(read_handle);
        CloseHandle(write_handle);
        return CUP_ERR_FILESYSTEM;
    }
    CloseHandle(read_handle);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    g_parent_signal = write_handle;
#else
    int pipe_fds[2];
    pid_t pid;

    if (pipe(pipe_fds) != 0)
        return CUP_ERR_FILESYSTEM;
    pid = fork();
    if (pid < 0) {
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        return CUP_ERR_FILESYSTEM;
    }
    if (pid == 0) {
        char descriptor[32];
        close(pipe_fds[1]);
        if (text_format(descriptor, sizeof(descriptor), "%d", pipe_fds[0]) != CUP_OK) {
            _exit(127);
        }
        execl(helper, helper, "--internal-cup-update-helper", token, descriptor, (char *)NULL);
        _exit(127);
    }
    close(pipe_fds[0]);
    g_parent_signal = pipe_fds[1];
#endif
    return CUP_OK;
}

/* Detached helper execution. The parent handshake, token and global lock are all verified before
 * committing assets. */
CupError cup_update_helper_run(const char *token, const char *wait_value) {
    CupUpdateJournal journal;
    CupUpdateJournalStatus status;
    SystemLock lock = {0};
    char staging[MAX_PATH_LEN];
    CupError err;

    err = wait_for_parent(wait_value);
    if (err != CUP_OK)
        return err;
    err = cup_update_journal_load(&journal, &status);
    if (err != CUP_OK || status != CUP_UPDATE_JOURNAL_LOADED || strcmp(journal.token, token) != 0 ||
        journal.phase != CUP_UPDATE_PHASE_SCHEDULED) {
        return CUP_ERR_TRANSACTION;
    }
    err = cup_update_journal_get_staging_path(&journal, staging, sizeof(staging));
    if (err != CUP_OK)
        return err;

    err = acquire_helper_lock(&lock);
    if (err != CUP_OK) {
        record_helper_failure(&journal, err, 0);
        return err;
    }
    err = commit_update(&journal, staging);
    if (err != CUP_OK) {
        record_helper_failure(&journal, err, 1);
    }
    system_lock_release(&lock);
    return err;
}
