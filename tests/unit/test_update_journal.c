/* Exercises the minimal CUP-generation journal and byte-evidence recovery protocol. */

#include "checksum.h"
#include "constants.h"
#include "generation.h"
#include "layout.h"
#include "path.h"
#include "release_metadata.h"
#include "system.h"
#include "test_platform.h"
#include "unity.h"
#include "update_journal.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char temp_dir[CUP_TEST_TEMP_PATH_SIZE];

static void write_file(const char *path, const char *text) {
    FILE *file = fopen(path, "wb");
    TEST_ASSERT_NOT_NULL(file);
    TEST_ASSERT_EQUAL_size_t(strlen(text), fwrite(text, 1, strlen(text), file));
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
}

static void read_file(const char *path, char *buffer, size_t size) {
    FILE *file = fopen(path, "rb");
    size_t count;
    TEST_ASSERT_NOT_NULL(file);
    count = fread(buffer, 1, size - 1u, file);
    TEST_ASSERT_EQUAL_INT(0, ferror(file));
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
    buffer[count] = '\0';
}

static void assert_text(const char *path, const char *expected) {
    char actual[256];
    read_file(path, actual, sizeof(actual));
    TEST_ASSERT_EQUAL_STRING(expected, actual);
}

static void generation_path(char *path,
                            size_t size,
                            const char *directory,
                            GenerationAssetId id) {
    char name[MAX_PATH_SEGMENT_LEN];
    TEST_ASSERT_EQUAL_INT(CUP_OK, generation_asset_release_name(id, name, sizeof(name)));
    TEST_ASSERT_EQUAL_INT(CUP_OK, path_join(path, size, directory, name));
}

static void write_release(const char *directory,
                          const char *version,
                          const char *license_text,
                          const char *notices_text,
                          const char *binary_text) {
    char license[MAX_PATH_LEN];
    char notices[MAX_PATH_LEN];
    char binary[MAX_PATH_LEN];
    char release[MAX_PATH_LEN];
    char binary_name[MAX_PATH_SEGMENT_LEN];
    char license_sha[65];
    char notices_sha[65];
    char binary_sha[65];
    char text[2048];

    generation_path(license, sizeof(license), directory, CUP_GENERATION_ASSET_LICENSE);
    generation_path(notices, sizeof(notices), directory, CUP_GENERATION_ASSET_NOTICES);
    generation_path(binary, sizeof(binary), directory, CUP_GENERATION_ASSET_BINARY);
    generation_path(release, sizeof(release), directory, CUP_GENERATION_ASSET_RELEASE);
    TEST_ASSERT_EQUAL_INT(CUP_OK,
                          generation_binary_release_name(binary_name, sizeof(binary_name)));
    write_file(license, license_text);
    write_file(notices, notices_text);
    write_file(binary, binary_text);
    TEST_ASSERT_EQUAL_INT(CUP_OK, checksum_sha256_file(license, license_sha, sizeof(license_sha)));
    TEST_ASSERT_EQUAL_INT(CUP_OK, checksum_sha256_file(notices, notices_sha, sizeof(notices_sha)));
    TEST_ASSERT_EQUAL_INT(CUP_OK, checksum_sha256_file(binary, binary_sha, sizeof(binary_sha)));
    TEST_ASSERT_TRUE(snprintf(text,
                              sizeof(text),
                              "format=2\n"
                              "version=%s\n"
                              "commit=0123456789abcdef0123456789abcdef01234567\n"
                              "root_layout=2\n"
                              "catalog_format=1\n"
                              "asset_count=3\n"
                              "asset.0.name=LICENSE\n"
                              "asset.0.sha256=%s\n"
                              "asset.1.name=THIRD_PARTY_NOTICES.txt\n"
                              "asset.1.sha256=%s\n"
                              "asset.2.name=%s\n"
                              "asset.2.sha256=%s\n",
                              version,
                              license_sha,
                              notices_sha,
                              binary_name,
                              binary_sha) > 0);
    write_file(release, text);
}

static void write_canonical_generation(const char *version,
                                       const char *license,
                                       const char *notices,
                                       const char *binary) {
    GenerationAssetSpec specs[CUP_GENERATION_ASSET_COUNT];
    char scratch[MAX_PATH_LEN];
    char directory[MAX_PATH_LEN];
    GenerationAssetId id;

    TEST_ASSERT_EQUAL_INT(CUP_OK, generation_asset_specs(specs));
    TEST_ASSERT_EQUAL_INT(CUP_OK, path_parent(directory, sizeof(directory), specs[0].destination));
    write_release(directory, version, license, notices, binary);
    /* write_release placed the binary release-name at root, while canonical binary lives in bin/. */
    generation_path(scratch, sizeof(scratch), directory, CUP_GENERATION_ASSET_BINARY);
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_copy_file(scratch, specs[CUP_GENERATION_ASSET_BINARY].destination));
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_remove_file(scratch));
    for (id = CUP_GENERATION_ASSET_LICENSE; id < CUP_GENERATION_ASSET_BINARY; ++id) {
        (void)id;
    }
}

static void create_target_workspace(char *staging,
                                    size_t staging_size,
                                    char *new_dir,
                                    size_t new_size,
                                    char digest[65]) {
    char staging_root[MAX_PATH_LEN];
    SystemCommitState commit_state = SYSTEM_COMMIT_NOT_APPLIED;
    char release[MAX_PATH_LEN];

    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_get_staging_dir(staging_root, sizeof(staging_root)));
    TEST_ASSERT_EQUAL_INT(CUP_OK,
                          system_create_temp_directory(
                              staging_root, CUP_UPDATE_TEMP_PREFIX, staging, staging_size));
    TEST_ASSERT_EQUAL_INT(CUP_OK,
                          path_join(new_dir, new_size, staging, CUP_UPDATE_NEW_DIRECTORY));
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_create_private_directory(new_dir, &commit_state));
    TEST_ASSERT_EQUAL_INT(SYSTEM_COMMIT_DURABLE, commit_state);
    write_release(new_dir, "2.0.0", "new-license", "new-notices", "new-binary");
    generation_path(release, sizeof(release), new_dir, CUP_GENERATION_ASSET_RELEASE);
    TEST_ASSERT_EQUAL_INT(CUP_OK, checksum_sha256_file(release, digest, 65));
}

static void copy_target_asset(const char *new_dir, GenerationAssetId id) {
    GenerationAssetSpec spec;
    char source[MAX_PATH_LEN];
    TEST_ASSERT_EQUAL_INT(CUP_OK, generation_asset_spec(id, &spec));
    generation_path(source, sizeof(source), new_dir, id);
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_copy_file(source, spec.destination));
}

static void begin_transaction(UpdateJournal *journal,
                              char *staging,
                              size_t staging_size,
                              char *new_dir,
                              size_t new_size) {
    char digest[65];
    create_target_workspace(staging, staging_size, new_dir, new_size, digest);
    TEST_ASSERT_EQUAL_INT(CUP_OK, update_generation_prepare(staging, digest));
    TEST_ASSERT_EQUAL_INT(CUP_OK, update_journal_begin(staging, digest, journal));
}

void setUp(void) {
    char lock_path[MAX_PATH_LEN];
    SystemLock lock;

    memset(&lock, 0, sizeof(lock));
    TEST_ASSERT_NOT_NULL(test_make_temp_directory(temp_dir, sizeof(temp_dir), "cup-update-journal"));
    TEST_ASSERT_EQUAL_INT(0, test_set_home(temp_dir));
    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_root_snapshot_begin());
    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_ensure_root());
    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_ensure_runtime());
    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_ensure_assets());
    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_get_lock_path(lock_path, sizeof(lock_path)));
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_lock_acquire(&lock, lock_path, SYSTEM_LOCK_EXCLUSIVE));
    system_lock_release(&lock);
    write_canonical_generation("1.0.0", "old-license", "old-notices", "old-binary");
}

void tearDown(void) {
    layout_root_snapshot_end();
    TEST_ASSERT_EQUAL_INT(0, test_remove_tree(temp_dir));
}

static void test_journal_schema_and_commit(void) {
    UpdateJournal journal;
    UpdateJournal loaded;
    UpdateJournalStatus status;
    char staging[MAX_PATH_LEN];
    char new_dir[MAX_PATH_LEN];
    char transaction[MAX_PATH_LEN];
    char contents[1024];
    GenerationAssetSpec spec;

    update_journal_init(&journal);
    begin_transaction(&journal, staging, sizeof(staging), new_dir, sizeof(new_dir));
    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_get_transaction_path(transaction, sizeof(transaction)));
    read_file(transaction, contents, sizeof(contents));
    TEST_ASSERT_NOT_NULL(strstr(contents, "format=2\noperation=cup-generation\n"));
    TEST_ASSERT_NOT_NULL(strstr(contents, "target_release_sha256="));
    TEST_ASSERT_NOT_NULL(strstr(contents, "temporary_name=cup-update-"));
    TEST_ASSERT_NULL(strstr(contents, "phase="));
    TEST_ASSERT_NULL(strstr(contents, "token="));
    TEST_ASSERT_NULL(strstr(contents, "version="));

    update_journal_init(&loaded);
    TEST_ASSERT_EQUAL_INT(CUP_OK, update_journal_load(&loaded, &status));
    TEST_ASSERT_EQUAL_INT(CUP_UPDATE_JOURNAL_LOADED, status);
    TEST_ASSERT_EQUAL_STRING(journal.temporary_name, loaded.temporary_name);
    TEST_ASSERT_EQUAL_STRING(journal.target_release_sha256, loaded.target_release_sha256);

    TEST_ASSERT_EQUAL_INT(CUP_OK, update_generation_commit(&loaded));
    TEST_ASSERT_EQUAL_INT(CUP_OK, generation_asset_spec(CUP_GENERATION_ASSET_LICENSE, &spec));
    assert_text(spec.destination, "new-license");
    TEST_ASSERT_EQUAL_INT(CUP_OK, generation_asset_spec(CUP_GENERATION_ASSET_BINARY, &spec));
    assert_text(spec.destination, "new-binary");
    TEST_ASSERT_EQUAL_INT(CUP_OK, update_journal_load(&loaded, &status));
    TEST_ASSERT_EQUAL_INT(CUP_UPDATE_JOURNAL_MISSING, status);
}

static void test_recovery_rolls_back_every_pre_binary_commit_boundary(void) {
    static const GenerationAssetId order[] = {
        CUP_GENERATION_ASSET_LICENSE,
        CUP_GENERATION_ASSET_NOTICES,
        CUP_GENERATION_ASSET_RELEASE};
    size_t committed;

    for (committed = 0; committed <= sizeof(order) / sizeof(order[0]); ++committed) {
        UpdateJournal journal;
        UpdateJournalStatus status;
        char staging[MAX_PATH_LEN];
        char new_dir[MAX_PATH_LEN];
        GenerationAssetSpec spec;
        size_t i;
        int finalized = -1;

        update_journal_init(&journal);
        begin_transaction(&journal, staging, sizeof(staging), new_dir, sizeof(new_dir));
        for (i = 0; i < committed; ++i) {
            copy_target_asset(new_dir, order[i]);
        }
        TEST_ASSERT_EQUAL_INT(CUP_OK, update_journal_recover(&journal, &finalized));
        TEST_ASSERT_EQUAL_INT(0, finalized);
        TEST_ASSERT_EQUAL_INT(CUP_OK, generation_asset_spec(CUP_GENERATION_ASSET_LICENSE, &spec));
        assert_text(spec.destination, "old-license");
        TEST_ASSERT_EQUAL_INT(CUP_OK, generation_asset_spec(CUP_GENERATION_ASSET_NOTICES, &spec));
        assert_text(spec.destination, "old-notices");
        TEST_ASSERT_EQUAL_INT(CUP_OK, generation_asset_spec(CUP_GENERATION_ASSET_BINARY, &spec));
        assert_text(spec.destination, "old-binary");
        TEST_ASSERT_EQUAL_INT(CUP_OK, update_journal_load(&journal, &status));
        TEST_ASSERT_EQUAL_INT(CUP_UPDATE_JOURNAL_MISSING, status);
    }
}

static void test_recovery_finalizes_complete_target(void) {
    UpdateJournal journal;
    UpdateJournalStatus status;
    char staging[MAX_PATH_LEN];
    char new_dir[MAX_PATH_LEN];
    GenerationAssetId id;
    int finalized = 0;

    update_journal_init(&journal);
    begin_transaction(&journal, staging, sizeof(staging), new_dir, sizeof(new_dir));
    for (id = CUP_GENERATION_ASSET_RELEASE; id < CUP_GENERATION_ASSET_COUNT; ++id) {
        copy_target_asset(new_dir, id);
    }
    TEST_ASSERT_EQUAL_INT(CUP_OK, update_journal_recover(&journal, &finalized));
    TEST_ASSERT_EQUAL_INT(1, finalized);
    TEST_ASSERT_EQUAL_INT(CUP_OK, update_journal_load(&journal, &status));
    TEST_ASSERT_EQUAL_INT(CUP_UPDATE_JOURNAL_MISSING, status);
}

static void test_recovery_preserves_ambiguous_binary(void) {
    UpdateJournal journal;
    UpdateJournal loaded;
    UpdateJournalStatus status;
    char staging[MAX_PATH_LEN];
    char new_dir[MAX_PATH_LEN];
    GenerationAssetSpec spec;
    int finalized = 0;

    update_journal_init(&journal);
    begin_transaction(&journal, staging, sizeof(staging), new_dir, sizeof(new_dir));
    TEST_ASSERT_EQUAL_INT(CUP_OK, generation_asset_spec(CUP_GENERATION_ASSET_BINARY, &spec));
    write_file(spec.destination, "third-binary");
    TEST_ASSERT_EQUAL_INT(CUP_ERR_TRANSACTION, update_journal_recover(&journal, &finalized));
    TEST_ASSERT_EQUAL_INT(0, finalized);
    assert_text(spec.destination, "third-binary");
    update_journal_init(&loaded);
    TEST_ASSERT_EQUAL_INT(CUP_OK, update_journal_load(&loaded, &status));
    TEST_ASSERT_EQUAL_INT(CUP_UPDATE_JOURNAL_LOADED, status);
}

static void test_old_journal_shape_is_rejected(void) {
    char transaction[MAX_PATH_LEN];
    UpdateJournal journal;
    UpdateJournalStatus status;

    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_get_transaction_path(transaction, sizeof(transaction)));
    write_file(transaction,
               "format=1\noperation=cup-update\nphase=scheduled\nversion=2.0.0\n");
    update_journal_init(&journal);
    TEST_ASSERT_EQUAL_INT(CUP_ERR_TRANSACTION, update_journal_load(&journal, &status));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_journal_schema_and_commit);
    RUN_TEST(test_recovery_rolls_back_every_pre_binary_commit_boundary);
    RUN_TEST(test_recovery_finalizes_complete_target);
    RUN_TEST(test_recovery_preserves_ambiguous_binary);
    RUN_TEST(test_old_journal_shape_is_rejected);
    return UNITY_END();
}
