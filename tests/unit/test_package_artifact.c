/*
 * Exercises immutable artifact coordinates, opened-file ownership, verification
 * decisions and identity-bound discard without depending on the filesystem or libarchive.
 */

#include "package_artifact.h"
#include "checksum.h"
#include "unity.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static const char DIGEST_A[] =
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
static const char DIGEST_B[] =
    "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";

static int version_available;
static int format_available;
static CupError catalog_result;
static CupError path_kind_result;
static SystemPathKind path_kind;
static CupError open_result;
static int open_missing;
static uint64_t open_size;
static CupError checksum_result;
static const char *calculated_digest;
static CupError remove_result;
static size_t remove_calls;
static SystemPathIdentity removed_identity;
static char removed_path[MAX_PATH_LEN];

static void make_identity(PackageIdentity *identity) {
    memset(identity, 0, sizeof(*identity));
    strcpy(identity->component, "compiler");
    strcpy(identity->tool, "gcc");
    strcpy(identity->host_platform, "linux-x64");
    strcpy(identity->target_platform, "linux-x64");
    strcpy(identity->version, "15.2.0");
}

static void make_catalog(PackageCatalog *catalog) {
    memset(catalog, 0, sizeof(*catalog));
    catalog->identity.volume = 7;
    catalog->identity.object = 11;
    catalog->identity.kind = SYSTEM_PATH_REGULAR_FILE;
    catalog->identity.valid = 1;
    strcpy(catalog->digest, DIGEST_A);
}

static void make_spec(PackageArtifactSpec *spec) {
    memset(spec, 0, sizeof(*spec));
    make_identity(&spec->identity);
    spec->format = PACKAGE_ARCHIVE_FORMAT_TAR_XZ;
    strcpy(spec->package_url, "https://example.invalid/gcc-15.2.0.tar.xz");
    strcpy(spec->checksum_url, "https://example.invalid/SHA256SUMS");
}

static FILE *make_open_file(void) {
    FILE *file = tmpfile();

    TEST_ASSERT_NOT_NULL(file);
    TEST_ASSERT_EQUAL_size_t(4, fwrite("data", 1, 4, file));
    rewind(file);
    return file;
}

void setUp(void) {
    version_available = 1;
    format_available = 1;
    catalog_result = CUP_OK;
    path_kind_result = CUP_OK;
    path_kind = SYSTEM_PATH_REGULAR_FILE;
    open_result = CUP_OK;
    open_missing = 0;
    open_size = 4;
    checksum_result = CUP_OK;
    calculated_digest = DIGEST_A;
    remove_result = CUP_OK;
    remove_calls = 0;
    memset(&removed_identity, 0, sizeof(removed_identity));
    removed_path[0] = '\0';
}

void tearDown(void) {
}

CupError package_identity_validate(const PackageIdentity *identity, FILE *diagnostics) {
    (void)diagnostics;
    return identity != NULL && identity->component[0] != '\0' && identity->tool[0] != '\0' &&
                   identity->host_platform[0] != '\0' && identity->target_platform[0] != '\0' &&
                   identity->version[0] != '\0'
               ? CUP_OK
               : CUP_ERR_INVALID_INPUT;
}

CupError package_identity_init(PackageIdentity *identity,
                               const char *component,
                               const char *tool,
                               const char *host_platform,
                               const char *target_platform,
                               const char *version) {
    if (identity == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    memset(identity, 0, sizeof(*identity));
    snprintf(identity->component, sizeof(identity->component), "%s", component);
    snprintf(identity->tool, sizeof(identity->tool), "%s", tool);
    snprintf(identity->host_platform, sizeof(identity->host_platform), "%s", host_platform);
    snprintf(identity->target_platform, sizeof(identity->target_platform), "%s", target_platform);
    snprintf(identity->version, sizeof(identity->version), "%s", version);
    return CUP_OK;
}

CupError package_archive_parse_format(const char *value, PackageArchiveFormat *format) {
    if (value == NULL || format == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    if (strcmp(value, "tar.xz") != 0) {
        return CUP_ERR_VALIDATION;
    }
    *format = PACKAGE_ARCHIVE_FORMAT_TAR_XZ;
    return CUP_OK;
}

CupError package_catalog_resolve_stable(const PackageCatalog *catalog,
                                        char *buffer,
                                        size_t size,
                                        const char *component,
                                        const char *tool,
                                        const char *host_platform,
                                        const char *target_platform) {
    (void)catalog;
    (void)component;
    (void)tool;
    (void)host_platform;
    (void)target_platform;
    if (catalog_result != CUP_OK) {
        return catalog_result;
    }
    return snprintf(buffer, size, "%s", "15.2.0") < (int)size ? CUP_OK
                                                                 : CUP_ERR_BUFFER_TOO_SMALL;
}

CupError package_catalog_get_default_format(const PackageCatalog *catalog,
                                            char *buffer,
                                            size_t size,
                                            const char *component,
                                            const char *tool,
                                            const char *host_platform,
                                            const char *target_platform) {
    (void)catalog;
    (void)component;
    (void)tool;
    (void)host_platform;
    (void)target_platform;
    if (catalog_result != CUP_OK) {
        return catalog_result;
    }
    return snprintf(buffer, size, "%s", "tar.xz") < (int)size ? CUP_OK
                                                                 : CUP_ERR_BUFFER_TOO_SMALL;
}

CupError package_catalog_has_version(const PackageCatalog *catalog,
                                     const char *component,
                                     const char *tool,
                                     const char *host_platform,
                                     const char *target_platform,
                                     const char *version,
                                     int *is_available) {
    (void)catalog;
    (void)component;
    (void)tool;
    (void)host_platform;
    (void)target_platform;
    (void)version;
    if (catalog_result != CUP_OK) {
        return catalog_result;
    }
    *is_available = version_available;
    return CUP_OK;
}

CupError package_catalog_has_format(const PackageCatalog *catalog,
                                    const char *component,
                                    const char *tool,
                                    const char *host_platform,
                                    const char *target_platform,
                                    const char *format,
                                    int *is_supported) {
    (void)catalog;
    (void)component;
    (void)tool;
    (void)host_platform;
    (void)target_platform;
    (void)format;
    if (catalog_result != CUP_OK) {
        return catalog_result;
    }
    *is_supported = format_available;
    return CUP_OK;
}

CupError package_catalog_build_url(const PackageCatalog *catalog,
                                   char *buffer,
                                   size_t size,
                                   const char *component,
                                   const char *tool,
                                   const char *host_platform,
                                   const char *target_platform,
                                   const char *version,
                                   const char *format) {
    (void)catalog;
    (void)component;
    (void)host_platform;
    (void)target_platform;
    return snprintf(buffer, size, "https://example.invalid/%s-%s.%s", tool, version, format) <
                   (int)size
               ? CUP_OK
               : CUP_ERR_BUFFER_TOO_SMALL;
}

CupError package_catalog_build_checksum_url(const PackageCatalog *catalog,
                                            char *buffer,
                                            size_t size,
                                            const char *component,
                                            const char *tool,
                                            const char *host_platform,
                                            const char *target_platform,
                                            const char *version) {
    (void)catalog;
    (void)component;
    (void)tool;
    (void)host_platform;
    (void)target_platform;
    (void)version;
    return snprintf(buffer, size, "%s", "https://example.invalid/SHA256SUMS") < (int)size
               ? CUP_OK
               : CUP_ERR_BUFFER_TOO_SMALL;
}

CupError layout_build_cache_archive_path(char *buffer,
                                         size_t size,
                                         const PackageIdentity *identity,
                                         const char *format) {
    return snprintf(buffer, size, "/cache/%s-%s.%s", identity->tool, identity->version, format) <
                   (int)size
               ? CUP_OK
               : CUP_ERR_BUFFER_TOO_SMALL;
}

CupError system_get_path_kind(const char *path, SystemPathKind *kind) {
    (void)path;
    if (path_kind_result == CUP_OK) {
        *kind = path_kind;
    }
    return path_kind_result;
}

CupError system_open_regular_file(const char *path,
                                  FILE **file,
                                  SystemPathIdentity *identity,
                                  uint64_t *file_size,
                                  int *missing) {
    (void)path;
    if (open_result != CUP_OK) {
        return open_result;
    }
    *missing = open_missing;
    if (open_missing) {
        return CUP_OK;
    }
    *file = make_open_file();
    identity->volume = 17;
    identity->object = 23;
    identity->kind = SYSTEM_PATH_REGULAR_FILE;
    identity->valid = 1;
    *file_size = open_size;
    return CUP_OK;
}

int checksum_digest_is_canonical(const char *value) {
    size_t i;

    if (value == NULL || strlen(value) != CHECKSUM_SHA256_HEX_LENGTH) {
        return 0;
    }
    for (i = 0; i < CHECKSUM_SHA256_HEX_LENGTH; ++i) {
        char c = value[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            return 0;
        }
    }
    return 1;
}

CupError checksum_sha256_stream(FILE *file, char *hex, size_t size) {
    TEST_ASSERT_NOT_NULL(file);
    if (checksum_result != CUP_OK) {
        return checksum_result;
    }
    TEST_ASSERT_TRUE(size >= CHECKSUM_SHA256_HEX_LENGTH + 1);
    strcpy(hex, calculated_digest);
    rewind(file);
    return CUP_OK;
}

CupError system_remove_file_if_identity(const char *path,
                                        const SystemPathIdentity *expected_identity) {
    remove_calls++;
    snprintf(removed_path, sizeof(removed_path), "%s", path);
    removed_identity = *expected_identity;
    return remove_result;
}

static void test_spec_build_pins_catalog_snapshot_and_urls(void) {
    PackageArtifactSpec spec;
    PackageCatalog catalog;
    PackageIdentity identity;

    make_catalog(&catalog);
    make_identity(&identity);

    TEST_ASSERT_EQUAL(CUP_OK,
                      package_artifact_spec_build(&spec, &catalog, &identity, "tar.xz"));
    TEST_ASSERT_EQUAL_INT(PACKAGE_ARCHIVE_FORMAT_TAR_XZ, spec.format);
    TEST_ASSERT_EQUAL_STRING("https://example.invalid/gcc-15.2.0.tar.xz", spec.package_url);
    TEST_ASSERT_EQUAL_STRING("https://example.invalid/SHA256SUMS", spec.checksum_url);
    TEST_ASSERT_EQUAL_STRING(identity.component, spec.identity.component);
    TEST_ASSERT_EQUAL_STRING(identity.tool, spec.identity.tool);
    TEST_ASSERT_EQUAL_STRING(identity.host_platform, spec.identity.host_platform);
    TEST_ASSERT_EQUAL_STRING(identity.target_platform, spec.identity.target_platform);
    TEST_ASSERT_EQUAL_STRING(identity.version, spec.identity.version);

    version_available = 0;
    TEST_ASSERT_EQUAL(CUP_ERR_NOT_AVAILABLE,
                      package_artifact_spec_build(&spec, &catalog, &identity, "tar.xz"));
    TEST_ASSERT_EQUAL_CHAR('\0', spec.identity.component[0]);
    TEST_ASSERT_EQUAL_INT(PACKAGE_ARCHIVE_FORMAT_ANY, spec.format);
    version_available = 1;
    format_available = 0;
    TEST_ASSERT_EQUAL(CUP_ERR_NOT_AVAILABLE,
                      package_artifact_spec_build(&spec, &catalog, &identity, "tar.xz"));
    TEST_ASSERT_EQUAL_CHAR('\0', spec.package_url[0]);
    format_available = 1;

    memset(catalog.digest, 'A', CHECKSUM_SHA256_HEX_LENGTH);
    catalog.digest[CHECKSUM_SHA256_HEX_LENGTH] = '\0';
    memset(&spec, 0x7f, sizeof(spec));
    TEST_ASSERT_EQUAL(CUP_ERR_INVALID_INPUT,
                      package_artifact_spec_build(&spec, &catalog, &identity, "tar.xz"));
    TEST_ASSERT_EQUAL_CHAR('\0', spec.identity.component[0]);
    TEST_ASSERT_EQUAL_INT(PACKAGE_ARCHIVE_FORMAT_ANY, spec.format);
    TEST_ASSERT_EQUAL_CHAR('\0', spec.package_url[0]);
    TEST_ASSERT_EQUAL_CHAR('\0', spec.checksum_url[0]);
}

static void test_stable_resolution_builds_concrete_spec(void) {
    PackageArtifactSpec spec;
    PackageCatalog catalog;

    make_catalog(&catalog);
    TEST_ASSERT_EQUAL(CUP_OK,
                      package_artifact_spec_resolve_stable(&spec,
                                                           &catalog,
                                                           "compiler",
                                                           "gcc",
                                                           "linux-x64",
                                                           "linux-x64"));
    TEST_ASSERT_EQUAL_STRING("15.2.0", spec.identity.version);
    TEST_ASSERT_EQUAL_INT(PACKAGE_ARCHIVE_FORMAT_TAR_XZ, spec.format);

    catalog_result = CUP_ERR_CATALOG;
    TEST_ASSERT_EQUAL(CUP_ERR_CATALOG,
                      package_artifact_spec_resolve_stable(&spec,
                                                           &catalog,
                                                           "compiler",
                                                           "gcc",
                                                           "linux-x64",
                                                           "linux-x64"));
    TEST_ASSERT_EQUAL_CHAR('\0', spec.identity.component[0]);
    TEST_ASSERT_EQUAL_CHAR('\0', spec.package_url[0]);
}

static void test_open_reports_non_file_states_without_opening(void) {
    PackageArtifactSpec spec;
    VerifiedArtifact artifact;
    ArtifactVerificationStatus status;

    make_spec(&spec);
    verified_artifact_init(&artifact);

    {
        char invalid_digest[CHECKSUM_SHA256_HEX_LENGTH + 1];
        memset(invalid_digest, 'g', CHECKSUM_SHA256_HEX_LENGTH);
        invalid_digest[CHECKSUM_SHA256_HEX_LENGTH] = '\0';
        TEST_ASSERT_EQUAL(
            CUP_ERR_INVALID_INPUT,
            verified_artifact_open(&artifact, "/cache/a", &spec, invalid_digest, &status));
    }

    path_kind = SYSTEM_PATH_MISSING;
    TEST_ASSERT_EQUAL(CUP_OK,
                      verified_artifact_open(&artifact, "/cache/a", &spec, DIGEST_A, &status));
    TEST_ASSERT_EQUAL(ARTIFACT_VERIFY_MISSING, status);
    TEST_ASSERT_NULL(artifact.file);

    path_kind = SYSTEM_PATH_DIRECTORY;
    TEST_ASSERT_EQUAL(CUP_OK,
                      verified_artifact_open(&artifact, "/cache/a", &spec, DIGEST_A, &status));
    TEST_ASSERT_EQUAL(ARTIFACT_VERIFY_WRONG_TYPE, status);
    TEST_ASSERT_NULL(artifact.file);
}

static void test_open_reports_size_and_digest_decisions(void) {
    PackageArtifactSpec spec;
    VerifiedArtifact artifact;
    ArtifactVerificationStatus status;

    make_spec(&spec);
    verified_artifact_init(&artifact);

    open_size = 0;
    TEST_ASSERT_EQUAL(CUP_OK,
                      verified_artifact_open(&artifact, "/cache/a", &spec, DIGEST_A, &status));
    TEST_ASSERT_EQUAL(ARTIFACT_VERIFY_REJECTED, status);

    open_size = MAX_PACKAGE_DOWNLOAD_BYTES + 1;
    TEST_ASSERT_EQUAL(CUP_OK,
                      verified_artifact_open(&artifact, "/cache/a", &spec, DIGEST_A, &status));
    TEST_ASSERT_EQUAL(ARTIFACT_VERIFY_REJECTED, status);

    open_size = 4;
    calculated_digest = DIGEST_B;
    TEST_ASSERT_EQUAL(CUP_OK,
                      verified_artifact_open(&artifact, "/cache/a", &spec, DIGEST_A, &status));
    TEST_ASSERT_EQUAL(ARTIFACT_VERIFY_DIGEST_MISMATCH, status);
    TEST_ASSERT_NOT_NULL(artifact.file);
    verified_artifact_release(&artifact);

}

static void test_valid_artifact_remains_bound_to_opened_bytes(void) {
    PackageArtifactSpec spec;
    VerifiedArtifact artifact;
    ArtifactVerificationStatus status;

    make_spec(&spec);
    verified_artifact_init(&artifact);

    TEST_ASSERT_EQUAL(CUP_OK,
                      verified_artifact_open(&artifact, "/cache/a", &spec, DIGEST_A, &status));
    TEST_ASSERT_EQUAL(ARTIFACT_VERIFY_VALID, status);
    TEST_ASSERT_EQUAL_STRING(DIGEST_A, artifact.digest);

    {
        char invalid_digest[CHECKSUM_SHA256_HEX_LENGTH + 1];
        memset(invalid_digest, 'G', CHECKSUM_SHA256_HEX_LENGTH);
        invalid_digest[CHECKSUM_SHA256_HEX_LENGTH] = '\0';
        TEST_ASSERT_EQUAL(CUP_ERR_INVALID_INPUT,
                          verified_artifact_verify_expected(&artifact, invalid_digest, &status));
        TEST_ASSERT_NOT_NULL(artifact.file);
    }

    TEST_ASSERT_EQUAL(CUP_OK,
                      verified_artifact_verify_expected(&artifact, DIGEST_B, &status));
    TEST_ASSERT_EQUAL(ARTIFACT_VERIFY_DIGEST_MISMATCH, status);
    TEST_ASSERT_NOT_NULL(artifact.file);

    verified_artifact_release(&artifact);
    TEST_ASSERT_NULL(artifact.file);
}

static void test_verification_io_error_releases_owned_stream(void) {
    PackageArtifactSpec spec;
    VerifiedArtifact artifact;
    ArtifactVerificationStatus status;

    make_spec(&spec);
    verified_artifact_init(&artifact);
    checksum_result = CUP_ERR_FILESYSTEM;

    TEST_ASSERT_EQUAL(CUP_ERR_FILESYSTEM,
                      verified_artifact_open(&artifact, "/cache/a", &spec, DIGEST_A, &status));
    TEST_ASSERT_EQUAL(ARTIFACT_VERIFY_NONE, status);
    TEST_ASSERT_NULL(artifact.file);
}


static void test_discard_uses_opened_identity(void) {
    PackageArtifactSpec spec;
    VerifiedArtifact artifact;
    ArtifactVerificationStatus status;

    make_spec(&spec);
    verified_artifact_init(&artifact);
    TEST_ASSERT_EQUAL(CUP_OK,
                      verified_artifact_open(&artifact, "/cache/a", &spec, DIGEST_A, &status));

    TEST_ASSERT_EQUAL(CUP_OK, verified_artifact_discard(&artifact));
    TEST_ASSERT_EQUAL_size_t(1, remove_calls);
    TEST_ASSERT_EQUAL_STRING("/cache/a", removed_path);
    TEST_ASSERT_EQUAL_UINT64(17, removed_identity.volume);
    TEST_ASSERT_EQUAL_UINT64(23, removed_identity.object);
    TEST_ASSERT_NULL(artifact.file);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_spec_build_pins_catalog_snapshot_and_urls);
    RUN_TEST(test_stable_resolution_builds_concrete_spec);
    RUN_TEST(test_open_reports_non_file_states_without_opening);
    RUN_TEST(test_open_reports_size_and_digest_decisions);
    RUN_TEST(test_valid_artifact_remains_bound_to_opened_bytes);
    RUN_TEST(test_verification_io_error_releases_owned_stream);
    RUN_TEST(test_discard_uses_opened_identity);
    return UNITY_END();
}
