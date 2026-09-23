/* Exercises the cup-generation trust, pinning and detached-handoff boundaries. */

#include "checksum.h"
#include "command_context.h"
#include "constants.h"
#include "download.h"
#include "filesystem.h"
#include "generation.h"
#include "interrupt.h"
#include "layout.h"
#include "release_metadata.h"
#include "runtime_journal.h"
#include "self_update.h"
#include "system.h"
#include "text.h"
#include "update_helper.h"
#include "update_journal.h"
#include "unity.h"
#include "version.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DIGEST_CURRENT "1111111111111111111111111111111111111111111111111111111111111111"
#define DIGEST_LICENSE "2222222222222222222222222222222222222222222222222222222222222222"
#define DIGEST_NOTICES "3333333333333333333333333333333333333333333333333333333333333333"
#define DIGEST_BINARY  "4444444444444444444444444444444444444444444444444444444444444444"
#define DIGEST_RELEASE "5555555555555555555555555555555555555555555555555555555555555555"
#define DIGEST_BAD     "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"

static char latest_version[CUP_RELEASE_VERSION_MAX];
static char latest_commit[CUP_RELEASE_COMMIT_MAX];
static char target_version[CUP_RELEASE_VERSION_MAX];
static char target_commit[CUP_RELEASE_COMMIT_MAX];
static const char *current_binary_digest;
static const char *target_license_digest;
static const char *target_notices_digest;
static const char *target_binary_digest;
static CupError current_manifest_result;
static CupError latest_manifest_result;
static CupError target_manifest_result;
static CupError download_result;
static int fail_download_call;
static CupError generation_prepare_result;
static CupError helper_prepare_result;
static CupError journal_begin_result;
static CupError safe_point_result;
static CupError helper_start_result;
static CupError journal_clear_result;
static CupError cleanup_result;

static int context_begin_calls;
static int context_end_calls;
static int temp_directory_calls;
static int ensure_directory_calls;
static int download_calls;
static int release_load_calls;
static int generation_prepare_calls;
static int helper_prepare_calls;
static int journal_begin_calls;
static int safe_point_calls;
static int helper_start_calls;
static int journal_clear_calls;
static int cleanup_calls;
static int executable_calls;
static char urls[8][MAX_CATALOG_URL_LEN];
static DownloadValidation validations[8];

static const ReleaseAsset current_binary_asset = {"cup-current", DIGEST_CURRENT};
static const ReleaseAsset target_license_asset = {"LICENSE", DIGEST_LICENSE};
static const ReleaseAsset target_notices_asset = {"THIRD_PARTY_NOTICES.txt", DIGEST_NOTICES};
static const ReleaseAsset target_binary_asset = {"cup-target", DIGEST_BINARY};

static CupError copy_text(char *buffer, size_t size, const char *value) {
    size_t length = strlen(value);
    if (length >= size) return CUP_ERR_BUFFER_TOO_SMALL;
    memcpy(buffer, value, length + 1);
    return CUP_OK;
}

static void set_metadata(ReleaseMetadata *metadata,
                         const char *version,
                         const char *commit,
                         unsigned marker) {
    memset(metadata, 0, sizeof(*metadata));
    TEST_ASSERT_EQUAL_INT(CUP_OK, copy_text(metadata->version, sizeof(metadata->version), version));
    TEST_ASSERT_EQUAL_INT(CUP_OK, copy_text(metadata->commit, sizeof(metadata->commit), commit));
    metadata->root_layout = marker;
}

static void reset_scenario(void) {
    strcpy(latest_version, "1.2.4");
    strcpy(latest_commit, "abcdef1234567890abcdef1234567890abcdef12");
    strcpy(target_version, "1.2.4");
    strcpy(target_commit, "abcdef1234567890abcdef1234567890abcdef12");
    current_binary_digest = DIGEST_CURRENT;
    target_license_digest = DIGEST_LICENSE;
    target_notices_digest = DIGEST_NOTICES;
    target_binary_digest = DIGEST_BINARY;
    current_manifest_result = CUP_OK;
    latest_manifest_result = CUP_OK;
    target_manifest_result = CUP_OK;
    download_result = CUP_ERR_FETCH;
    fail_download_call = 0;
    generation_prepare_result = CUP_OK;
    helper_prepare_result = CUP_OK;
    journal_begin_result = CUP_OK;
    safe_point_result = CUP_OK;
    helper_start_result = CUP_OK;
    journal_clear_result = CUP_OK;
    cleanup_result = CUP_OK;
    context_begin_calls = 0;
    context_end_calls = 0;
    temp_directory_calls = 0;
    ensure_directory_calls = 0;
    download_calls = 0;
    release_load_calls = 0;
    generation_prepare_calls = 0;
    helper_prepare_calls = 0;
    journal_begin_calls = 0;
    safe_point_calls = 0;
    helper_start_calls = 0;
    journal_clear_calls = 0;
    cleanup_calls = 0;
    executable_calls = 0;
    memset(urls, 0, sizeof(urls));
    memset(validations, 0, sizeof(validations));
}

void setUp(void) { reset_scenario(); }
void tearDown(void) {}

CupError command_context_begin(CommandContext *context,
                               const char *target_override,
                               SystemLockMode mode) {
    TEST_ASSERT_NOT_NULL(context);
    TEST_ASSERT_NULL(target_override);
    TEST_ASSERT_EQUAL_INT(SYSTEM_LOCK_EXCLUSIVE, mode);
    memset(context, 0, sizeof(*context));
    context->lock.handle = 7;
    context->lock.mode = SYSTEM_LOCK_EXCLUSIVE;
    context->lock.active = 1;
    context_begin_calls++;
    return CUP_OK;
}

void command_context_end(CommandContext *context) {
    TEST_ASSERT_NOT_NULL(context);
    context_end_calls++;
}

CupError layout_get_root(char *buffer, size_t size) {
    return copy_text(buffer, size, "/mock/root");
}

CupError layout_get_staging_dir(char *buffer, size_t size) {
    return copy_text(buffer, size, "/mock/root/tmp");
}

static const char *binary_release_name(void) {
#if defined(_WIN32)
    return "cup-windows-x64.exe";
#elif defined(__APPLE__) && defined(__aarch64__)
    return "cup-macos-arm64";
#elif defined(__APPLE__)
    return "cup-macos-x64";
#elif defined(__aarch64__)
    return "cup-linux-arm64";
#else
    return "cup-linux-x64";
#endif
}

CupError generation_asset_spec(GenerationAssetId id, GenerationAssetSpec *spec) {
    const char *name = NULL;
    const char *destination = NULL;
    TEST_ASSERT_NOT_NULL(spec);
    memset(spec, 0, sizeof(*spec));
    spec->id = id;
    switch (id) {
        case CUP_GENERATION_ASSET_RELEASE:
            name = "release.txt";
            destination = "/mock/root/release.txt";
            spec->read_only = 1;
            break;
        case CUP_GENERATION_ASSET_LICENSE:
            name = "LICENSE";
            destination = "/mock/root/LICENSE";
            spec->read_only = 1;
            break;
        case CUP_GENERATION_ASSET_NOTICES:
            name = "THIRD_PARTY_NOTICES.txt";
            destination = "/mock/root/THIRD_PARTY_NOTICES.txt";
            spec->read_only = 1;
            break;
        case CUP_GENERATION_ASSET_BINARY:
            name = binary_release_name();
            destination = "/mock/root/bin/cup";
            spec->executable = 1;
            break;
        default:
            return CUP_ERR_INVALID_INPUT;
    }
    TEST_ASSERT_EQUAL_INT(CUP_OK, copy_text(spec->release_name, sizeof(spec->release_name), name));
    TEST_ASSERT_EQUAL_INT(CUP_OK, copy_text(spec->destination, sizeof(spec->destination), destination));
    return CUP_OK;
}

CupError generation_asset_specs(GenerationAssetSpec specs[CUP_GENERATION_ASSET_COUNT]) {
    size_t i;
    TEST_ASSERT_NOT_NULL(specs);
    for (i = 0; i < CUP_GENERATION_ASSET_COUNT; ++i) {
        CupError err = generation_asset_spec((GenerationAssetId)i, &specs[i]);
        if (err != CUP_OK) return err;
    }
    return CUP_OK;
}

void release_metadata_init(ReleaseMetadata *metadata) {
    if (metadata != NULL) memset(metadata, 0, sizeof(*metadata));
}

void release_metadata_free(ReleaseMetadata *metadata) {
    if (metadata != NULL) memset(metadata, 0, sizeof(*metadata));
}

CupError release_metadata_load(const char *path, ReleaseMetadata *metadata) {
    TEST_ASSERT_NOT_NULL(path);
    TEST_ASSERT_NOT_NULL(metadata);
    release_load_calls++;
    if (strcmp(path, "/mock/root/release.txt") == 0) {
        set_metadata(metadata, CUP_VERSION_BASE,
                     "0123456789012345678901234567890123456789", 1);
        return CUP_OK;
    }
    if (strstr(path, "latest-release.txt") != NULL) {
        set_metadata(metadata, latest_version, latest_commit, 2);
        return CUP_OK;
    }
    if (strstr(path, "/new/release.txt") != NULL) {
        set_metadata(metadata, target_version, target_commit, 3);
        return CUP_OK;
    }
    TEST_FAIL_MESSAGE("unexpected release metadata path");
    return CUP_ERR_VALIDATION;
}

CupError release_version_parse(const char *text, ReleaseVersion *version) {
    unsigned major, minor, patch;
    char extra;
    if (text == NULL || version == NULL ||
        sscanf(text, "%u.%u.%u%c", &major, &minor, &patch, &extra) != 3) {
        return CUP_ERR_INVALID_INPUT;
    }
    version->major = major;
    version->minor = minor;
    version->patch = patch;
    return CUP_OK;
}

CupError generation_validate_manifest(const ReleaseMetadata *metadata) {
    TEST_ASSERT_NOT_NULL(metadata);
    if (metadata->root_layout == 1) return current_manifest_result;
    if (metadata->root_layout == 2) return latest_manifest_result;
    if (metadata->root_layout == 3) return target_manifest_result;
    return CUP_ERR_VALIDATION;
}

const ReleaseAsset *generation_manifest_asset(const ReleaseMetadata *metadata,
                                              GenerationAssetId id) {
    static ReleaseAsset license_asset;
    static ReleaseAsset notices_asset;
    static ReleaseAsset binary_asset;
    TEST_ASSERT_NOT_NULL(metadata);
    if (metadata->root_layout == 1 && id == CUP_GENERATION_ASSET_BINARY) {
        return &current_binary_asset;
    }
    if (metadata->root_layout != 3) return NULL;
    if (id == CUP_GENERATION_ASSET_LICENSE) {
        license_asset = target_license_asset;
        strcpy(license_asset.sha256, DIGEST_LICENSE);
        return &license_asset;
    }
    if (id == CUP_GENERATION_ASSET_NOTICES) {
        notices_asset = target_notices_asset;
        strcpy(notices_asset.sha256, DIGEST_NOTICES);
        return &notices_asset;
    }
    if (id == CUP_GENERATION_ASSET_BINARY) {
        binary_asset = target_binary_asset;
        strcpy(binary_asset.sha256, DIGEST_BINARY);
        return &binary_asset;
    }
    return NULL;
}

int checksum_digest_is_canonical(const char *digest) {
    size_t i;
    if (digest == NULL || strlen(digest) != 64) return 0;
    for (i = 0; i < 64; ++i) {
        if (!((digest[i] >= '0' && digest[i] <= '9') ||
              (digest[i] >= 'a' && digest[i] <= 'f'))) return 0;
    }
    return 1;
}

CupError checksum_sha256_file(const char *path, char *digest, size_t size) {
    const char *value = NULL;
    TEST_ASSERT_NOT_NULL(path);
    if (strcmp(path, "/mock/root/bin/cup") == 0) value = current_binary_digest;
    else if (strstr(path, "/new/LICENSE") != NULL) value = target_license_digest;
    else if (strstr(path, "/new/THIRD_PARTY_NOTICES.txt") != NULL) value = target_notices_digest;
    else if (strstr(path, "/new/") != NULL && strstr(path, "cup-") != NULL) value = target_binary_digest;
    else {
        TEST_FAIL_MESSAGE("unexpected digest path");
        return CUP_ERR_VALIDATION;
    }
    return copy_text(digest, size, value);
}

CupError system_create_temp_directory(const char *directory,
                                      const char *prefix,
                                      char *path,
                                      size_t path_size) {
    TEST_ASSERT_EQUAL_STRING("/mock/root/tmp", directory);
    TEST_ASSERT_EQUAL_STRING(CUP_UPDATE_TEMP_PREFIX, prefix);
    temp_directory_calls++;
    return copy_text(path, path_size, "/mock/root/tmp/cup-update-abc");
}

CupError filesystem_ensure_directory(const char *path) {
    TEST_ASSERT_EQUAL_STRING("/mock/root/tmp/cup-update-abc/new", path);
    ensure_directory_calls++;
    return CUP_OK;
}

CupError download_copy_release_base_override(char *base, size_t size) {
    return copy_text(base, size, "https://example.test/cup");
}

CupError download_file(const char *url, const char *destination, DownloadValidation validation) {
    TEST_ASSERT_NOT_NULL(url);
    TEST_ASSERT_NOT_NULL(destination);
    TEST_ASSERT_TRUE(download_calls < (int)(sizeof(urls) / sizeof(urls[0])));
    strcpy(urls[download_calls], url);
    validations[download_calls] = validation;
    download_calls++;
    if (fail_download_call == download_calls) return download_result;
    return CUP_OK;
}

CupError system_set_executable(const char *path, int executable) {
    TEST_ASSERT_NOT_NULL(path);
    TEST_ASSERT_TRUE(executable);
    executable_calls++;
    return CUP_OK;
}

CupError update_generation_prepare(const char *staging, char target_release_sha256[65]) {
    TEST_ASSERT_EQUAL_STRING("/mock/root/tmp/cup-update-abc", staging);
    generation_prepare_calls++;
    TEST_ASSERT_EQUAL_INT(CUP_OK, copy_text(target_release_sha256, 65, DIGEST_RELEASE));
    return generation_prepare_result;
}

CupError update_helper_prepare_from(const char *source_binary) {
    TEST_ASSERT_EQUAL_STRING("/mock/root/bin/cup", source_binary);
    helper_prepare_calls++;
    return helper_prepare_result;
}

unsigned long system_get_process_id(void) { return 1234; }

void update_journal_init(UpdateJournal *journal) {
    if (journal != NULL) memset(journal, 0, sizeof(*journal));
}

CupError update_journal_begin(const char *temporary_path,
                              const char *target_release_sha256,
                              UpdateJournal *created) {
    TEST_ASSERT_EQUAL_STRING("/mock/root/tmp/cup-update-abc", temporary_path);
    TEST_ASSERT_EQUAL_STRING(DIGEST_RELEASE, target_release_sha256);
    TEST_ASSERT_NOT_NULL(created);
    journal_begin_calls++;
    memset(created, 0, sizeof(*created));
    if (journal_begin_result == CUP_OK || journal_begin_result == CUP_ERR_COMMIT) {
        created->file_identity.valid = 1;
        created->file_identity.kind = SYSTEM_PATH_REGULAR_FILE;
        strcpy(created->temporary_name, "cup-update-abc");
        strcpy(created->target_release_sha256, DIGEST_RELEASE);
    }
    return journal_begin_result;
}

CupError interrupt_safe_point(void) {
    safe_point_calls++;
    return safe_point_result;
}

CupError update_helper_start(const char *root, const char *token, SystemLock *lock) {
    TEST_ASSERT_EQUAL_STRING("/mock/root", root);
    TEST_ASSERT_EQUAL_STRING("u1234-cup-update-abc", token);
    TEST_ASSERT_NOT_NULL(lock);
    TEST_ASSERT_TRUE(lock->active);
    TEST_ASSERT_EQUAL_INT(SYSTEM_LOCK_EXCLUSIVE, lock->mode);
    helper_start_calls++;
    if (helper_start_result == CUP_OK) lock->active = 0;
    return helper_start_result;
}

CupError runtime_journal_clear_if_identity(const SystemPathIdentity *identity) {
    TEST_ASSERT_NOT_NULL(identity);
    TEST_ASSERT_TRUE(identity->valid);
    journal_clear_calls++;
    return journal_clear_result;
}

CupError filesystem_remove_tree(const char *path) {
    TEST_ASSERT_EQUAL_STRING("/mock/root/tmp/cup-update-abc", path);
    cleanup_calls++;
    return cleanup_result;
}

static void assert_no_handoff(void) {
    TEST_ASSERT_EQUAL_INT(0, generation_prepare_calls);
    TEST_ASSERT_EQUAL_INT(0, helper_prepare_calls);
    TEST_ASSERT_EQUAL_INT(0, journal_begin_calls);
    TEST_ASSERT_EQUAL_INT(0, helper_start_calls);
}

static void test_current_binary_must_be_trusted_before_staging(void) {
    current_binary_digest = DIGEST_BAD;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_VALIDATION, self_update_start());
    TEST_ASSERT_EQUAL_INT(1, context_begin_calls);
    TEST_ASSERT_EQUAL_INT(1, release_load_calls);
    TEST_ASSERT_EQUAL_INT(0, temp_directory_calls);
    TEST_ASSERT_EQUAL_INT(0, download_calls);
    TEST_ASSERT_EQUAL_INT(0, cleanup_calls);
    assert_no_handoff();
}

static void test_equal_and_older_release_are_local_noops_after_discovery(void) {
    strcpy(latest_version, CUP_VERSION_BASE);
    TEST_ASSERT_EQUAL_INT(CUP_OK, self_update_start());
    TEST_ASSERT_EQUAL_INT(1, download_calls);
    TEST_ASSERT_EQUAL_STRING("https://example.test/cup/release.txt", urls[0]);
    TEST_ASSERT_EQUAL_INT(DOWNLOAD_VALIDATE_METADATA, validations[0]);
    TEST_ASSERT_EQUAL_INT(1, cleanup_calls);
    assert_no_handoff();

    reset_scenario();
    strcpy(latest_version, "1.2.2");
    TEST_ASSERT_EQUAL_INT(CUP_OK, self_update_start());
    TEST_ASSERT_EQUAL_INT(1, download_calls);
    TEST_ASSERT_EQUAL_INT(1, cleanup_calls);
    assert_no_handoff();
}

static void test_newer_release_is_pinned_then_handed_off(void) {
    TEST_ASSERT_EQUAL_INT(CUP_OK, self_update_start());
    TEST_ASSERT_EQUAL_INT(5, download_calls);
    TEST_ASSERT_EQUAL_STRING("https://example.test/cup/release.txt", urls[0]);
    TEST_ASSERT_EQUAL_STRING("https://example.test/cup/1.2.4/release.txt", urls[1]);
    TEST_ASSERT_EQUAL_STRING("https://example.test/cup/1.2.4/LICENSE", urls[2]);
    TEST_ASSERT_EQUAL_STRING("https://example.test/cup/1.2.4/THIRD_PARTY_NOTICES.txt", urls[3]);
    TEST_ASSERT_NOT_NULL(strstr(urls[4], "https://example.test/cup/1.2.4/cup-"));
    TEST_ASSERT_EQUAL_INT(DOWNLOAD_VALIDATE_BINARY, validations[4]);
    TEST_ASSERT_EQUAL_INT(1, generation_prepare_calls);
    TEST_ASSERT_EQUAL_INT(1, helper_prepare_calls);
    TEST_ASSERT_EQUAL_INT(1, journal_begin_calls);
    TEST_ASSERT_EQUAL_INT(1, safe_point_calls);
    TEST_ASSERT_EQUAL_INT(1, helper_start_calls);
    TEST_ASSERT_EQUAL_INT(0, journal_clear_calls);
    TEST_ASSERT_EQUAL_INT(0, cleanup_calls);
#if defined(_WIN32)
    TEST_ASSERT_EQUAL_INT(0, executable_calls);
#else
    TEST_ASSERT_EQUAL_INT(1, executable_calls);
#endif
}

static void test_versioned_manifest_must_match_discovery_identity(void) {
    strcpy(target_version, "1.2.5");
    TEST_ASSERT_EQUAL_INT(CUP_ERR_VALIDATION, self_update_start());
    TEST_ASSERT_EQUAL_INT(2, download_calls);
    TEST_ASSERT_EQUAL_STRING("https://example.test/cup/1.2.4/release.txt", urls[1]);
    TEST_ASSERT_EQUAL_INT(1, cleanup_calls);
    assert_no_handoff();
}

static void test_target_asset_digest_failure_stops_before_transaction(void) {
    target_binary_digest = DIGEST_BAD;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_VALIDATION, self_update_start());
    TEST_ASSERT_EQUAL_INT(5, download_calls);
    TEST_ASSERT_EQUAL_INT(1, cleanup_calls);
    assert_no_handoff();
}

static void test_helper_failure_cancels_parent_owned_transaction(void) {
    helper_start_result = CUP_ERR_TEMPORARY;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_TEMPORARY, self_update_start());
    TEST_ASSERT_EQUAL_INT(1, generation_prepare_calls);
    TEST_ASSERT_EQUAL_INT(1, journal_begin_calls);
    TEST_ASSERT_EQUAL_INT(1, helper_start_calls);
    TEST_ASSERT_EQUAL_INT(1, journal_clear_calls);
    TEST_ASSERT_EQUAL_INT(1, cleanup_calls);
}

static void test_cleanup_failure_after_handoff_failure_reports_transaction_error(void) {
    helper_start_result = CUP_ERR_TEMPORARY;
    journal_clear_result = CUP_ERR_COMMIT;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_TRANSACTION, self_update_start());
    TEST_ASSERT_EQUAL_INT(1, journal_clear_calls);
    TEST_ASSERT_EQUAL_INT(0, cleanup_calls);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_current_binary_must_be_trusted_before_staging);
    RUN_TEST(test_equal_and_older_release_are_local_noops_after_discovery);
    RUN_TEST(test_newer_release_is_pinned_then_handed_off);
    RUN_TEST(test_versioned_manifest_must_match_discovery_identity);
    RUN_TEST(test_target_asset_digest_failure_stops_before_transaction);
    RUN_TEST(test_helper_failure_cancels_parent_owned_transaction);
    RUN_TEST(test_cleanup_failure_after_handoff_failure_reports_transaction_error);
    return UNITY_END();
}
