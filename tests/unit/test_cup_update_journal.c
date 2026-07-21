#define _POSIX_C_SOURCE 200809L
/* Exercises the strict deferred CUP update journal and persisted result. */

#include "cup_assets.h"
#include "cup_update_journal.h"
#include "filesystem.h"
#include "layout.h"
#include "path.h"
#include "system.h"
#include "unity.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/*
 * Scenario controls and observations. Configured results drive the boundary doubles below;
 * counters record the calls made by production code.
 */

static char root[MAX_PATH_LEN];
static CupError replace_result;
static SystemCommitState replace_state;
static CupError permission_result;
static int cup_assets_valid;
static int remove_tree_calls;

/* Fixture lifecycle and local construction helpers. */

static CupError buffer_write_result(int written, size_t size) {
    return written >= 0 && (size_t)written < size ? CUP_OK : CUP_ERR_BUFFER_TOO_SMALL;
}

static void join_test_path(char *buffer, size_t size, const char *left, const char *right) {
    int written = snprintf(buffer, size, "%s/%s", left, right);

    TEST_ASSERT_TRUE(written >= 0 && (size_t)written < size);
}

static void remove_tree_real(const char *path) {
    DIR *directory = opendir(path);
    struct dirent *entry;

    if (directory == NULL) {
        (void)unlink(path);
        return;
    }
    while ((entry = readdir(directory)) != NULL) {
        char child[MAX_PATH_LEN];

        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        join_test_path(child, sizeof(child), path, entry->d_name);
        remove_tree_real(child);
    }
    (void)closedir(directory);
    (void)rmdir(path);
}

static void make_dir(const char *path) {
    TEST_ASSERT_TRUE(mkdir(path, 0700) == 0 || errno == EEXIST);
}

static void write_file(const char *path, const char *text) {
    FILE *file = fopen(path, "wb");

    TEST_ASSERT_NOT_NULL(file);
    TEST_ASSERT_EQUAL_size_t(strlen(text), fwrite(text, 1, strlen(text), file));
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
}

static void assert_file_text(const char *path, const char *expected) {
    char buffer[64];
    FILE *file = fopen(path, "rb");
    size_t length;

    TEST_ASSERT_NOT_NULL(file);
    length = fread(buffer, 1, sizeof(buffer) - 1, file);
    TEST_ASSERT_EQUAL_INT(0, ferror(file));
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
    buffer[length] = '\0';
    TEST_ASSERT_EQUAL_STRING(expected, buffer);
}

static void reset_scenario(void) {
    char template_path[] = "/tmp/cup-update-journal-unit-XXXXXX";
    char staging[MAX_PATH_LEN];
    char bin[MAX_PATH_LEN];
    char config[MAX_PATH_LEN];

    TEST_ASSERT_NOT_NULL(mkdtemp(template_path));
    strcpy(root, template_path);
    join_test_path(staging, sizeof(staging), root, "staging");
    join_test_path(bin, sizeof(bin), root, "bin");
    join_test_path(config, sizeof(config), root, "config");
    make_dir(staging);
    make_dir(bin);
    make_dir(config);

    replace_result = CUP_OK;
    replace_state = SYSTEM_COMMIT_DURABLE;
    permission_result = CUP_OK;
    cup_assets_valid = 1;
    remove_tree_calls = 0;
}

void setUp(void) {
    reset_scenario();
}

void tearDown(void) {
    remove_tree_real(root);
}

/*
 * Controlled boundary doubles. Each implementation exposes one dependency through the scenario
 * state above.
 */

CupError layout_get_root(char *buffer, size_t size) {
    return buffer_write_result(snprintf(buffer, size, "%s", root), size);
}

CupError layout_get_transaction_path(char *buffer, size_t size) {
    return path_join(buffer, size, root, "transaction.txt");
}

CupError layout_get_staging_dir(char *buffer, size_t size) {
    return path_join(buffer, size, root, "staging");
}

CupError layout_get_cup_update_result_path(char *buffer, size_t size) {
    return path_join(buffer, size, root, "cup-update-result.txt");
}

CupError layout_get_binary_path(char *buffer, size_t size) {
    return path_join(buffer, size, root, "bin/cup");
}

CupError layout_get_uninstall_path(char *buffer, size_t size) {
    return path_join(buffer, size, root, "bin/uninstall.sh");
}

CupError layout_get_platform_checksums_path(char *buffer, size_t size) {
    return path_join(buffer, size, root, "config/SHA256SUMS");
}

CupError layout_get_package_catalog_path(char *buffer, size_t size) {
    return path_join(buffer, size, root, "config/packages.cfg");
}

CupError layout_get_install_policy_path(char *buffer, size_t size) {
    return path_join(buffer, size, root, "config/install.cfg");
}

CupError layout_get_common_checksums_path(char *buffer, size_t size) {
    return path_join(buffer, size, root, "config/SHA256SUMS.common");
}

CupError system_create_temp_file(
    const char *directory, const char *prefix, char *path, size_t path_size, FILE **file) {
    int descriptor;
    int written;

    written = snprintf(path, path_size, "%s/%s-XXXXXX", directory, prefix);
    if (written < 0 || (size_t)written >= path_size) {
        return CUP_ERR_BUFFER_TOO_SMALL;
    }
    descriptor = mkstemp(path);
    if (descriptor < 0) {
        return CUP_ERR_TEMPORARY;
    }
    *file = fdopen(descriptor, "w+b");
    if (*file == NULL) {
        close(descriptor);
        unlink(path);
        return CUP_ERR_TEMPORARY;
    }
    return CUP_OK;
}

CupError system_sync_file(FILE *file) {
    return fflush(file) == 0 ? CUP_OK : CUP_ERR_FILESYSTEM;
}

CupError system_replace_file(const char *source,
                             const char *destination,
                             SystemCommitState *state) {
    *state = replace_state;
    if (replace_result != CUP_OK) {
        return replace_result;
    }
    return rename(source, destination) == 0 ? CUP_OK : CUP_ERR_FILESYSTEM;
}

CupError system_path_exists(const char *path, int *exists) {
    *exists = access(path, F_OK) == 0;
    return CUP_OK;
}

CupError system_remove_file(const char *path) {
    return unlink(path) == 0 || errno == ENOENT ? CUP_OK : CUP_ERR_FILESYSTEM;
}

CupError system_sync_parent_directory(const char *path) {
    (void)path;
    return CUP_OK;
}

CupError system_get_path_kind(const char *path, SystemPathKind *kind) {
    struct stat status;

    if (lstat(path, &status) != 0) {
        *kind = errno == ENOENT ? SYSTEM_PATH_MISSING : SYSTEM_PATH_OTHER;
        return errno == ENOENT ? CUP_OK : CUP_ERR_FILESYSTEM;
    }
    if (S_ISREG(status.st_mode)) {
        *kind = SYSTEM_PATH_REGULAR_FILE;
    } else if (S_ISDIR(status.st_mode)) {
        *kind = SYSTEM_PATH_DIRECTORY;
    } else if (S_ISLNK(status.st_mode)) {
        *kind = SYSTEM_PATH_LINK;
    } else {
        *kind = SYSTEM_PATH_OTHER;
    }
    return CUP_OK;
}

CupError system_set_executable(const char *path, int executable) {
    (void)path;
    (void)executable;
    return permission_result;
}

CupError system_set_read_only(const char *path, int read_only) {
    (void)path;
    (void)read_only;
    return permission_result;
}

CupError filesystem_remove_tree(const char *path) {
    remove_tree_calls++;
    remove_tree_real(path);
    return CUP_OK;
}

CupError cup_assets_inspect(CupAssetsInspection *inspection) {
    memset(inspection, 0, sizeof(*inspection));
    return CUP_OK;
}

int cup_assets_installed_is_valid(const CupAssetsInspection *inspection) {
    (void)inspection;
    return cup_assets_valid;
}

static void make_staging(const char *name, char *path, size_t size) {
    char staging[MAX_PATH_LEN];

    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_get_staging_dir(staging, sizeof(staging)));
    TEST_ASSERT_EQUAL_INT(CUP_OK, path_join(path, size, staging, name));
    make_dir(path);
}

static void write_journal(const char *text) {
    char path[MAX_PATH_LEN];

    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_get_transaction_path(path, sizeof(path)));
    write_file(path, text);
}

static void create_destination_files(void) {
    char path[MAX_PATH_LEN];

    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_get_binary_path(path, sizeof(path)));
    write_file(path, "new");
    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_get_uninstall_path(path, sizeof(path)));
    write_file(path, "new");
    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_get_platform_checksums_path(path, sizeof(path)));
    write_file(path, "new");
    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_get_package_catalog_path(path, sizeof(path)));
    write_file(path, "new");
    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_get_install_policy_path(path, sizeof(path)));
    write_file(path, "new");
    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_get_common_checksums_path(path, sizeof(path)));
    write_file(path, "new");
}

static void create_backups(const char *staging) {
    const char *names[] = {CUP_UPDATE_BINARY_OLD,
                           CUP_UPDATE_UNINSTALL_OLD,
                           CUP_UPDATE_PLATFORM_CHECKSUMS_OLD,
                           CUP_UPDATE_PACKAGES_OLD,
                           CUP_UPDATE_INSTALL_POLICY_OLD,
                           CUP_UPDATE_COMMON_CHECKSUMS_OLD};
    size_t i;

    for (i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        char path[MAX_PATH_LEN];
        TEST_ASSERT_EQUAL_INT(CUP_OK, path_join(path, sizeof(path), staging, names[i]));
        write_file(path, "old");
    }
}

/*
 * Test cases exercise the real production entry point while changing only controlled boundary
 * outcomes.
 */

static void test_model_and_begin(void) {
    CupUpdateJournal journal;
    CupUpdateJournalStatus status;
    char staging[MAX_PATH_LEN];
    char path[MAX_PATH_LEN];

    cup_update_journal_init(&journal);
    TEST_ASSERT_EQUAL_INT(CUP_UPDATE_PHASE_SCHEDULED, journal.phase);
    TEST_ASSERT_EQUAL_STRING("scheduled", cup_update_phase_name(CUP_UPDATE_PHASE_SCHEDULED));
    TEST_ASSERT_EQUAL_STRING("committing", cup_update_phase_name(CUP_UPDATE_PHASE_COMMITTING));
    TEST_ASSERT_EQUAL_STRING("failed", cup_update_phase_name(CUP_UPDATE_PHASE_FAILED));

    make_staging("cup-update-begin", staging, sizeof(staging));
    TEST_ASSERT_EQUAL_INT(CUP_OK, cup_update_journal_begin(staging, "token-1", "1.2.3"));
    TEST_ASSERT_EQUAL_INT(CUP_OK, cup_update_journal_load(&journal, &status));
    TEST_ASSERT_EQUAL_INT(CUP_UPDATE_JOURNAL_LOADED, status);
    TEST_ASSERT_EQUAL_STRING("cup-update-begin", journal.temporary_name);
    TEST_ASSERT_EQUAL_STRING("token-1", journal.token);
    TEST_ASSERT_EQUAL_STRING("1.2.3", journal.version);
    TEST_ASSERT_EQUAL_INT(CUP_UPDATE_PHASE_SCHEDULED, journal.phase);

    TEST_ASSERT_EQUAL_INT(CUP_OK,
                          cup_update_journal_get_staging_path(&journal, path, sizeof(path)));
    TEST_ASSERT_EQUAL_STRING(staging, path);
    TEST_ASSERT_EQUAL_INT(CUP_OK,
                          cup_update_journal_set_phase(&journal, CUP_UPDATE_PHASE_COMMITTING, 0));
    TEST_ASSERT_EQUAL_INT(CUP_OK, cup_update_journal_load(&journal, &status));
    TEST_ASSERT_EQUAL_INT(CUP_UPDATE_PHASE_COMMITTING, journal.phase);
    TEST_ASSERT_EQUAL_INT(CUP_OK,
                          cup_update_journal_set_phase(&journal, CUP_UPDATE_PHASE_FAILED, 19));
    TEST_ASSERT_EQUAL_INT(CUP_OK, cup_update_journal_load(&journal, &status));
    TEST_ASSERT_EQUAL_INT(CUP_UPDATE_PHASE_FAILED, journal.phase);
    TEST_ASSERT_EQUAL_INT(19, journal.error_code);

    TEST_ASSERT_EQUAL_INT(CUP_ERR_TRANSACTION,
                          cup_update_journal_begin(staging, "token-2", "1.2.4"));
    TEST_ASSERT_EQUAL_INT(CUP_OK, cup_update_journal_clear());
    TEST_ASSERT_EQUAL_INT(CUP_OK, cup_update_journal_load(&journal, &status));
    TEST_ASSERT_EQUAL_INT(CUP_UPDATE_JOURNAL_MISSING, status);

    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          cup_update_journal_begin("/tmp/self-update-old", "token", "1"));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          cup_update_journal_begin(staging, "bad token", "1"));
}

static void test_strict_load(void) {
    CupUpdateJournal journal;
    CupUpdateJournalStatus status;

    write_journal("journal_version=2\noperation=self-update\n"
                  "temporary_name=self-update-old\n");
    TEST_ASSERT_EQUAL_INT(CUP_ERR_TRANSACTION, cup_update_journal_load(&journal, &status));

    write_journal("format=1\noperation=cup-update\nphase=scheduled\n"
                  "temporary_name=cup-update-x\ntoken=t\nversion=1\nerror=1\n");
    TEST_ASSERT_EQUAL_INT(CUP_ERR_TRANSACTION, cup_update_journal_load(&journal, &status));

    write_journal("format=1\noperation=cup-update\nphase=failed\n"
                  "temporary_name=cup-update-x\ntoken=t\nversion=1\nerror=19\n");
    TEST_ASSERT_EQUAL_INT(CUP_OK, cup_update_journal_load(&journal, &status));
    TEST_ASSERT_EQUAL_INT(CUP_UPDATE_PHASE_FAILED, journal.phase);
}

static void test_persisted_result(void) {
    CupUpdateResult result;
    char path[MAX_PATH_LEN];

    cup_update_result_init(&result);
    TEST_ASSERT_EQUAL_INT(CUP_UPDATE_RESULT_MISSING, result.status);
    TEST_ASSERT_EQUAL_INT(CUP_OK, cup_update_result_load(&result));
    TEST_ASSERT_EQUAL_INT(CUP_UPDATE_RESULT_MISSING, result.status);

    TEST_ASSERT_EQUAL_INT(CUP_OK, cup_update_result_write(CUP_UPDATE_RESULT_SUCCESS, 0, "2.0.0"));
    TEST_ASSERT_EQUAL_INT(CUP_OK, cup_update_result_load(&result));
    TEST_ASSERT_EQUAL_INT(CUP_UPDATE_RESULT_SUCCESS, result.status);
    TEST_ASSERT_EQUAL_STRING("2.0.0", result.version);

    TEST_ASSERT_EQUAL_INT(CUP_OK, cup_update_result_write(CUP_UPDATE_RESULT_FAILED, 19, "2.0.1"));
    TEST_ASSERT_EQUAL_INT(CUP_OK, cup_update_result_load(&result));
    TEST_ASSERT_EQUAL_INT(CUP_UPDATE_RESULT_FAILED, result.status);
    TEST_ASSERT_EQUAL_INT(19, result.error_code);

    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_get_cup_update_result_path(path, sizeof(path)));
    write_file(path, "format=9\nstatus=success\nerror=0\nversion=1\n");
    TEST_ASSERT_EQUAL_INT(CUP_ERR_TRANSACTION, cup_update_result_load(&result));
}

static void test_recover_committed(void) {
    CupUpdateJournal journal;
    char staging[MAX_PATH_LEN];
    char marker[MAX_PATH_LEN];
    char journal_path[MAX_PATH_LEN];

    cup_update_journal_init(&journal);
    strcpy(journal.temporary_name, "cup-update-committed");
    make_staging(journal.temporary_name, staging, sizeof(staging));
    TEST_ASSERT_EQUAL_INT(CUP_OK, path_join(marker, sizeof(marker), staging, CUP_UPDATE_COMMITTED));
    write_file(marker, "ok");
    write_journal("journal");

    TEST_ASSERT_EQUAL_INT(CUP_OK, cup_update_journal_recover(&journal));
    TEST_ASSERT_EQUAL_INT(1, remove_tree_calls);
    TEST_ASSERT_TRUE(access(staging, F_OK) != 0);
    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_get_transaction_path(journal_path, sizeof(journal_path)));
    TEST_ASSERT_TRUE(access(journal_path, F_OK) != 0);
}

static void test_recover_rollback(void) {
    CupUpdateJournal journal;
    char staging[MAX_PATH_LEN];
    char path[MAX_PATH_LEN];

    cup_update_journal_init(&journal);
    strcpy(journal.temporary_name, "cup-update-rollback");
    make_staging(journal.temporary_name, staging, sizeof(staging));
    create_backups(staging);
    create_destination_files();
    write_journal("journal");

    TEST_ASSERT_EQUAL_INT(CUP_OK, cup_update_journal_recover(&journal));
    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_get_binary_path(path, sizeof(path)));
    assert_file_text(path, "old");
    TEST_ASSERT_TRUE(access(staging, F_OK) != 0);
}

/* Suite registration. */

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_model_and_begin);
    RUN_TEST(test_strict_load);
    RUN_TEST(test_persisted_result);
    RUN_TEST(test_recover_committed);
    RUN_TEST(test_recover_rollback);
    return UNITY_END();
}
