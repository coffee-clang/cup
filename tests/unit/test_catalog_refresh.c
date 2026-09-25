#include "catalog_refresh.h"
#include "command_context.h"
#include "download.h"
#include "filesystem.h"
#include "layout.h"
#include "package_catalog.h"
#include "system.h"
#include "unity.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_STEPS 8

typedef struct {
    unsigned long long revision;
    const char *digest;
    unsigned long long object;
} CatalogStep;

static CatalogStep local_steps[MAX_STEPS];
static size_t local_step_count;
static size_t local_step_index;
static CatalogStep remote_steps[MAX_STEPS];
static size_t remote_step_count;
static size_t remote_step_index;
static int lock_depth;
static int download_calls;
static int replace_calls;
static int remove_calls;
static CupError download_result;
static CupError replace_result;
static DownloadDiagnostics download_diagnostics;
static int runtime_available;
static int snapshot_missing;

static void copy_step(PackageCatalog *catalog, const CatalogStep *step) {
    memset(catalog, 0, sizeof(*catalog));
    catalog->revision = step->revision;
    strcpy(catalog->digest, step->digest);
    strcpy(catalog->update_url, "https://example.invalid/catalog.cfg");
    catalog->identity.valid = 1;
    catalog->identity.kind = SYSTEM_PATH_REGULAR_FILE;
    catalog->identity.volume = 1;
    catalog->identity.object = step->object;
}

static void push_local(unsigned long long revision, const char *digest, unsigned long long object) {
    TEST_ASSERT_TRUE(local_step_count < MAX_STEPS);
    local_steps[local_step_count++] = (CatalogStep){revision, digest, object};
}

static void push_remote(unsigned long long revision, const char *digest) {
    TEST_ASSERT_TRUE(remote_step_count < MAX_STEPS);
    remote_steps[remote_step_count++] = (CatalogStep){revision, digest, 99};
}

void setUp(void) {
    memset(local_steps, 0, sizeof(local_steps));
    memset(remote_steps, 0, sizeof(remote_steps));
    local_step_count = 0;
    local_step_index = 0;
    remote_step_count = 0;
    remote_step_index = 0;
    lock_depth = 0;
    download_calls = 0;
    replace_calls = 0;
    remove_calls = 0;
    download_result = CUP_OK;
    replace_result = CUP_OK;
    download_diagnostics = DOWNLOAD_DIAGNOSTICS_REPORT;
    runtime_available = 1;
    snapshot_missing = 0;
}

void tearDown(void) {
    TEST_ASSERT_EQUAL_INT(0, lock_depth);
}

void package_catalog_init(PackageCatalog *catalog) { memset(catalog, 0, sizeof(*catalog)); }
void package_catalog_free(PackageCatalog *catalog) { memset(catalog, 0, sizeof(*catalog)); }

CupError command_context_begin_read_only(CommandContext *context, const char *target_override) {
    (void)target_override;
    memset(context, 0, sizeof(*context));
    context->runtime_available = runtime_available;
    context->lock.active = 1;
    context->lock.mode = SYSTEM_LOCK_SHARED;
    lock_depth++;
    return CUP_OK;
}

CupError command_context_begin(CommandContext *context,
                               const char *target_override,
                               SystemLockMode mode) {
    (void)target_override;
    TEST_ASSERT_EQUAL_INT(SYSTEM_LOCK_EXCLUSIVE, mode);
    memset(context, 0, sizeof(*context));
    context->runtime_available = 1;
    context->lock.active = 1;
    context->lock.mode = mode;
    lock_depth++;
    return CUP_OK;
}

void command_context_end(CommandContext *context) {
    if (context->lock.active) {
        TEST_ASSERT_GREATER_THAN_INT(0, lock_depth);
        lock_depth--;
    }
    memset(context, 0, sizeof(*context));
}

CupError package_catalog_load_installed(PackageCatalog *catalog) {
    TEST_ASSERT_TRUE(local_step_index < local_step_count);
    copy_step(catalog, &local_steps[local_step_index++]);
    return CUP_OK;
}

CupError package_catalog_load_path(PackageCatalog *catalog, const char *path) {
    TEST_ASSERT_EQUAL_STRING("/config/catalog-refresh.tmp", path);
    TEST_ASSERT_TRUE(remote_step_index < remote_step_count);
    copy_step(catalog, &remote_steps[remote_step_index++]);
    return CUP_OK;
}

CupError layout_get_config_dir(char *buffer, size_t size) {
    return snprintf(buffer, size, "/config") < (int)size ? CUP_OK : CUP_ERR_BUFFER_TOO_SMALL;
}

CupError layout_get_package_catalog_path(char *buffer, size_t size) {
    return snprintf(buffer, size, "/config/catalog.cfg") < (int)size ? CUP_OK
                                                                     : CUP_ERR_BUFFER_TOO_SMALL;
}

CupError system_make_unique_temp_path(const char *directory,
                                      const char *prefix,
                                      char *path,
                                      size_t path_size) {
    TEST_ASSERT_EQUAL_STRING("/config", directory);
    TEST_ASSERT_EQUAL_STRING("catalog-refresh", prefix);
    return snprintf(path, path_size, "/config/catalog-refresh.tmp") < (int)path_size
               ? CUP_OK
               : CUP_ERR_BUFFER_TOO_SMALL;
}

CupError download_file_with_diagnostics(const char *url, const char *destination, DownloadValidation validation, DownloadDiagnostics diagnostics) {
    download_diagnostics = diagnostics;
    TEST_ASSERT_EQUAL_INT(0, lock_depth);
    TEST_ASSERT_EQUAL_STRING("https://example.invalid/catalog.cfg", url);
    TEST_ASSERT_EQUAL_STRING("/config/catalog-refresh.tmp", destination);
    TEST_ASSERT_EQUAL_INT(DOWNLOAD_VALIDATE_METADATA, validation);
    download_calls++;
    return download_result;
}

void filesystem_snapshot_init(PersistentFileSnapshot *snapshot) {
    memset(snapshot, 0, sizeof(*snapshot));
}

void filesystem_snapshot_release(PersistentFileSnapshot *snapshot) {
    free(snapshot->data);
    memset(snapshot, 0, sizeof(*snapshot));
}

CupError filesystem_snapshot_read(const char *path,
                                  size_t maximum_bytes,
                                  PersistentFileSnapshot *snapshot,
                                  int *missing) {
    static const unsigned char bytes[] = "remote-catalog\n";
    (void)maximum_bytes;
    TEST_ASSERT_EQUAL_STRING("/config/catalog-refresh.tmp", path);
    snapshot->data = malloc(sizeof(bytes) - 1u);
    TEST_ASSERT_NOT_NULL(snapshot->data);
    memcpy(snapshot->data, bytes, sizeof(bytes) - 1u);
    snapshot->size = sizeof(bytes) - 1u;
    *missing = snapshot_missing;
    return CUP_OK;
}

int system_path_identity_equal(const SystemPathIdentity *left,
                               const SystemPathIdentity *right) {
    return left->valid == right->valid && left->kind == right->kind &&
           left->volume == right->volume && left->object == right->object &&
           left->object_high == right->object_high;
}

CupError filesystem_replace_file_if_identity(const char *directory,
                                             const char *temporary_prefix,
                                             const char *destination,
                                             const SystemPathIdentity *expected_identity,
                                             int executable,
                                             FilesystemFileWriter writer,
                                             const void *value) {
    FILE *file;
    CupError write_result;

    TEST_ASSERT_NOT_NULL(writer);
    TEST_ASSERT_NOT_NULL(value);
    TEST_ASSERT_EQUAL_STRING("/config", directory);
    TEST_ASSERT_EQUAL_STRING("catalog", temporary_prefix);
    TEST_ASSERT_EQUAL_STRING("/config/catalog.cfg", destination);
    TEST_ASSERT_TRUE(expected_identity->valid);
    TEST_ASSERT_EQUAL_INT(0, executable);
    file = tmpfile();
    TEST_ASSERT_NOT_NULL(file);
    write_result = writer(file, value);
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
    TEST_ASSERT_EQUAL_INT(CUP_OK, write_result);
    replace_calls++;
    return replace_result;
}

CupError system_remove_file(const char *path) {
    TEST_ASSERT_EQUAL_STRING("/config/catalog-refresh.tmp", path);
    remove_calls++;
    return CUP_OK;
}

static void test_higher_revision_commits_without_network_lock(void) {
    int updated = 0;
    push_local(1, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", 1);
    push_local(1, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", 1);
    push_remote(2, "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");

    TEST_ASSERT_EQUAL_INT(CUP_OK, catalog_refresh_existing(&updated, CATALOG_REFRESH_REPORT_ERRORS));
    TEST_ASSERT_EQUAL_INT(1, updated);
    TEST_ASSERT_EQUAL_INT(1, download_calls);
    TEST_ASSERT_EQUAL_INT(DOWNLOAD_DIAGNOSTICS_REPORT, download_diagnostics);
    TEST_ASSERT_EQUAL_INT(1, replace_calls);
    TEST_ASSERT_EQUAL_INT(1, remove_calls);
}

static void test_quiet_refresh_forwards_quiet_download_diagnostics(void) {
    int updated = 0;
    push_local(1, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", 1);
    push_local(1, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", 1);
    push_remote(2, "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");

    TEST_ASSERT_EQUAL_INT(CUP_OK, catalog_refresh_existing(&updated, CATALOG_REFRESH_QUIET));
    TEST_ASSERT_EQUAL_INT(1, updated);
    TEST_ASSERT_EQUAL_INT(DOWNLOAD_DIAGNOSTICS_QUIET, download_diagnostics);
}

static void test_same_revision_same_bytes_is_noop(void) {
    int updated = 0;
    push_local(5, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", 1);
    push_local(5, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", 1);
    push_remote(5, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");

    TEST_ASSERT_EQUAL_INT(CUP_OK, catalog_refresh_existing(&updated, CATALOG_REFRESH_REPORT_ERRORS));
    TEST_ASSERT_EQUAL_INT(0, updated);
    TEST_ASSERT_EQUAL_INT(0, replace_calls);
}

static void test_same_revision_different_bytes_is_error(void) {
    int updated = 0;
    push_local(5, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", 1);
    push_local(5, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", 1);
    push_remote(5, "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");

    TEST_ASSERT_EQUAL_INT(CUP_ERR_CATALOG, catalog_refresh_existing(&updated, CATALOG_REFRESH_REPORT_ERRORS));
    TEST_ASSERT_EQUAL_INT(0, replace_calls);
}

static void test_lower_revision_is_rejected(void) {
    int updated = 0;
    push_local(5, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", 1);
    push_local(5, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", 1);
    push_remote(4, "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");

    TEST_ASSERT_EQUAL_INT(CUP_ERR_CATALOG, catalog_refresh_existing(&updated, CATALOG_REFRESH_REPORT_ERRORS));
    TEST_ASSERT_EQUAL_INT(0, replace_calls);
}

static void test_lost_cas_retries_from_new_snapshot(void) {
    int updated = 0;
    push_local(1, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", 1);
    push_local(2, "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb", 2);
    push_local(2, "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb", 2);
    push_local(2, "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb", 2);
    push_remote(3, "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc");
    push_remote(3, "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc");

    TEST_ASSERT_EQUAL_INT(CUP_OK, catalog_refresh_existing(&updated, CATALOG_REFRESH_REPORT_ERRORS));
    TEST_ASSERT_EQUAL_INT(1, updated);
    TEST_ASSERT_EQUAL_INT(2, download_calls);
    TEST_ASSERT_EQUAL_INT(1, replace_calls);
}

static void test_refresh_boundary_failures(void) {
    int updated = 7;

    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_INVALID_INPUT, catalog_refresh_existing(NULL, CATALOG_REFRESH_REPORT_ERRORS));
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_INVALID_INPUT, catalog_refresh_existing(&updated, (CatalogRefreshDiagnostics)99));

    updated = 7;
    runtime_available = 0;
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_NOT_INSTALLED, catalog_refresh_existing(&updated, CATALOG_REFRESH_REPORT_ERRORS));
    TEST_ASSERT_EQUAL_INT(0, updated);
    TEST_ASSERT_EQUAL_INT(0, download_calls);
}

static void test_download_failure_cleanup(void) {
    int updated = 0;

    push_local(1, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", 1);
    download_result = CUP_ERR_FETCH;
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_FETCH, catalog_refresh_existing(&updated, CATALOG_REFRESH_REPORT_ERRORS));
    TEST_ASSERT_EQUAL_INT(1, download_calls);
    TEST_ASSERT_EQUAL_INT(1, remove_calls);
    TEST_ASSERT_EQUAL_INT(0, replace_calls);
}

static void test_missing_snapshot_cleanup(void) {
    int updated = 0;

    push_local(1, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", 1);
    push_remote(2, "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");
    snapshot_missing = 1;
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_FILESYSTEM, catalog_refresh_existing(&updated, CATALOG_REFRESH_REPORT_ERRORS));
    TEST_ASSERT_EQUAL_INT(1, remove_calls);
    TEST_ASSERT_EQUAL_INT(0, replace_calls);
}

static void test_replace_failure_is_reported(void) {
    int updated = 0;

    push_local(1, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", 1);
    push_local(1, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", 1);
    push_remote(2, "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");
    replace_result = CUP_ERR_COMMIT;
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_COMMIT, catalog_refresh_existing(&updated, CATALOG_REFRESH_REPORT_ERRORS));
    TEST_ASSERT_EQUAL_INT(1, replace_calls);
    TEST_ASSERT_EQUAL_INT(0, updated);
}

static void test_repeated_cas_loss_is_bounded(void) {
    int updated = 0;
    unsigned i;
    for (i = 0; i < 3; ++i) {
        char digest_before[65];
        char digest_after[65];
        memset(digest_before, 'a' + (int)i, 64);
        memset(digest_after, 'd' + (int)i, 64);
        digest_before[64] = '\0';
        digest_after[64] = '\0';
        push_local(i + 1u, strdup(digest_before), i * 2u + 1u);
        push_local(i + 2u, strdup(digest_after), i * 2u + 2u);
        push_remote(10u, "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff");
    }

    TEST_ASSERT_EQUAL_INT(CUP_ERR_TEMPORARY, catalog_refresh_existing(&updated, CATALOG_REFRESH_REPORT_ERRORS));
    TEST_ASSERT_EQUAL_INT(3, download_calls);
    TEST_ASSERT_EQUAL_INT(0, replace_calls);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_higher_revision_commits_without_network_lock);
    RUN_TEST(test_quiet_refresh_forwards_quiet_download_diagnostics);
    RUN_TEST(test_same_revision_same_bytes_is_noop);
    RUN_TEST(test_same_revision_different_bytes_is_error);
    RUN_TEST(test_lower_revision_is_rejected);
    RUN_TEST(test_lost_cas_retries_from_new_snapshot);
    RUN_TEST(test_refresh_boundary_failures);
    RUN_TEST(test_download_failure_cleanup);
    RUN_TEST(test_missing_snapshot_cleanup);
    RUN_TEST(test_replace_failure_is_reported);
    RUN_TEST(test_repeated_cas_loss_is_bounded);
    return UNITY_END();
}
