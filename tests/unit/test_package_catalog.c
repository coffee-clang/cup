/* Exercises concrete catalog parsing, operational filtering and exact artifact queries. */

#include "package_catalog.h"
#include "layout.h"
#include "system.h"
#include "text.h"
#include "unity.h"
#include "test_platform.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *const SHA_A =
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
static const char *const SHA_B =
    "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";

static char temp_dir[CUP_TEST_TEMP_PATH_SIZE];
static char installed_path[MAX_PATH_LEN];
static CupError layout_error;
static CupError exists_error;
static int installed_exists;
static int development_exists;
static int exists_calls;
static int exists_error_call;

void setUp(void) {
    layout_error = CUP_OK;
    exists_error = CUP_OK;
    installed_exists = 0;
    development_exists = 0;
    exists_calls = 0;
    exists_error_call = 0;
    installed_path[0] = '\0';
}
void tearDown(void) {}

int download_insecure_loopback_is_allowed(const char *url) {
    (void)url;
    return 0;
}

int checksum_digest_is_canonical(const char *value) {
    size_t i;
    if (value == NULL || strlen(value) != 64u) return 0;
    for (i = 0; i < 64u; ++i) {
        if (!((value[i] >= '0' && value[i] <= '9') || (value[i] >= 'a' && value[i] <= 'f'))) return 0;
    }
    return 1;
}

CupError checksum_sha256_bytes(const unsigned char *data, size_t data_size, char *hex, size_t size) {
    (void)data; (void)data_size;
    if (hex == NULL || size < 65) return CUP_ERR_BUFFER_TOO_SMALL;
    memset(hex, 'd', 64); hex[64] = '\0'; return CUP_OK;
}


CupError layout_ensure_config(void) { return CUP_OK; }
CupError layout_get_package_catalog_path(char *buffer, size_t size) {
    if (layout_error != CUP_OK) return layout_error;
    return text_copy(buffer, size, installed_path);
}

CupError system_path_exists(const char *path, int *exists) {
    exists_calls++;
    if (exists_error != CUP_OK || exists_calls == exists_error_call) {
        return exists_error != CUP_OK ? exists_error : CUP_ERR_FILESYSTEM;
    }
    if (path == NULL || exists == NULL) return CUP_ERR_INVALID_INPUT;
    *exists = strcmp(path, installed_path) == 0 ? installed_exists : development_exists;
    return CUP_OK;
}

CupError system_copy_file(const char *source_path, const char *destination_path) {
    (void)source_path;
    (void)destination_path;
    return CUP_OK;
}

static void build_path(char *out, size_t size, const char *name) {
    int written = snprintf(out, size, "%s/%s", temp_dir, name);
    TEST_ASSERT_TRUE(written >= 0 && (size_t)written < size);
}

static void write_text(const char *path, const char *text) {
    FILE *file = fopen(path, "wb");
    TEST_ASSERT_NOT_NULL(file);
    TEST_ASSERT_EQUAL_size_t(strlen(text), fwrite(text, 1, strlen(text), file));
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
}

static void write_header(FILE *file, unsigned long long revision) {
    TEST_ASSERT_TRUE(fprintf(file,
                             "format=1\nrevision=%llu\n"
                             "update_url=https://raw.example.invalid/catalog.cfg\n",
                             revision) > 0);
}

static void write_package(FILE *file,
                          size_t index,
                          const char *component,
                          const char *tool,
                          const char *host,
                          const char *target,
                          const char *version,
                          int stable,
                          const char *reason,
                          const char *format,
                          const char *url,
                          const char *sha) {
    TEST_ASSERT_TRUE(fprintf(file,
                             "package.%zu.component=%s\n"
                             "package.%zu.tool=%s\n"
                             "package.%zu.host=%s\n"
                             "package.%zu.target=%s\n"
                             "package.%zu.version=%s\n"
                             "package.%zu.stable=%s\n",
                             index, component, index, tool, index, host, index, target,
                             index, version, index, stable ? "true" : "false") > 0);
    if (reason != NULL) {
        TEST_ASSERT_TRUE(fprintf(file, "package.%zu.revision_reason=%s\n", index, reason) > 0);
    }
    TEST_ASSERT_TRUE(fprintf(file,
                             "package.%zu.artifact.0.format=%s\n"
                             "package.%zu.artifact.0.url=%s\n"
                             "package.%zu.artifact.0.sha256=%s\n",
                             index, format, index, url, index, sha) > 0);
}

static void write_valid_catalog(const char *path) {
    FILE *file = fopen(path, "wb");
    TEST_ASSERT_NOT_NULL(file);
    write_header(file, 42);
    write_package(file, 0, "compiler", "clang", "linux-x64", "linux-x64", "23.1.0", 0,
                  NULL, "tar.gz", "https://example.invalid/clang-23.1.0.tar.gz", SHA_A);
    write_package(file, 1, "compiler", "clang", "linux-x64", "linux-x64", "24.0.0-rev1", 1,
                  "Packaging fix", "zip", "https://example.invalid/clang-24.0.0-rev1.zip", SHA_B);
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
}

static void assert_rejected(const char *name, const char *body) {
    PackageCatalog catalog;
    char path[256];
    package_catalog_init(&catalog);
    build_path(path, sizeof(path), name);
    write_text(path, body);
    TEST_ASSERT_EQUAL_INT(CUP_ERR_CATALOG, package_catalog_load_path(&catalog, path));
    TEST_ASSERT_EQUAL_size_t(0, catalog.count);
    package_catalog_free(&catalog);
}

static void test_load_queries_and_artifacts(void) {
    PackageCatalog catalog;
    char path[256];
    char value[MAX_CATALOG_URL_LEN];
    char digest[65];
    int flag = 0;

    package_catalog_init(&catalog);
    build_path(path, sizeof(path), "valid.cfg");
    write_valid_catalog(path);
    TEST_ASSERT_EQUAL_INT(CUP_OK, package_catalog_load_path(&catalog, path));
    TEST_ASSERT_EQUAL_size_t(2, catalog.count);
    TEST_ASSERT_EQUAL_UINT64(42, catalog.revision);
    TEST_ASSERT_TRUE(catalog.packages[0].operational);
    TEST_ASSERT_TRUE(catalog.packages[1].operational);
    TEST_ASSERT_EQUAL_STRING("Packaging fix", catalog.packages[1].revision_reason);

    TEST_ASSERT_EQUAL_INT(CUP_OK,
                          package_catalog_resolve_stable(&catalog, value, sizeof(value),
                                                         "compiler", "clang", "linux-x64", "linux-x64"));
    TEST_ASSERT_EQUAL_STRING("24.0.0-rev1", value);
    TEST_ASSERT_EQUAL_INT(CUP_OK,
                          package_catalog_is_stable(&catalog, "compiler", "clang", "linux-x64",
                                                    "linux-x64", "24.0.0-rev1", &flag));
    TEST_ASSERT_TRUE(flag);
    TEST_ASSERT_EQUAL_INT(CUP_OK,
                          package_catalog_has_version(&catalog, "compiler", "clang", "linux-x64",
                                                      "linux-x64", "23.1.0", &flag));
    TEST_ASSERT_TRUE(flag);
    TEST_ASSERT_EQUAL_INT(CUP_OK,
                          package_catalog_resolve_artifact(&catalog, "compiler", "clang",
                                                           "linux-x64", "linux-x64", "24.0.0-rev1",
                                                           "zip", value, sizeof(value), digest, sizeof(digest)));
    TEST_ASSERT_EQUAL_STRING("https://example.invalid/clang-24.0.0-rev1.zip", value);
    TEST_ASSERT_EQUAL_STRING(SHA_B, digest);
    package_catalog_free(&catalog);
}

static void test_future_records_are_structural_not_operational(void) {
    PackageCatalog catalog;
    char path[256];
    char stable[MAX_IDENTIFIER_LEN];
    int available = 1;
    FILE *file;

    package_catalog_init(&catalog);
    build_path(path, sizeof(path), "future.cfg");
    file = fopen(path, "wb"); TEST_ASSERT_NOT_NULL(file);
    write_header(file, 1);
    write_package(file, 0, "future-component", "future-tool", "future-os-riscv128",
                  "future-os-riscv128", "future-v1", 1, NULL, "future-format",
                  "https://example.invalid/future.pkg", SHA_A);
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
    TEST_ASSERT_EQUAL_INT(CUP_OK, package_catalog_load_path(&catalog, path));
    TEST_ASSERT_EQUAL_size_t(1, catalog.count);
    TEST_ASSERT_FALSE(catalog.packages[0].operational);
    TEST_ASSERT_EQUAL_INT(CUP_OK,
                          package_catalog_has_package(&catalog, "future-component", "future-tool",
                                                      "future-os-riscv128", "future-os-riscv128", &available));
    TEST_ASSERT_FALSE(available);
    TEST_ASSERT_EQUAL_INT(CUP_ERR_NOT_AVAILABLE,
                          package_catalog_resolve_stable(&catalog, stable, sizeof(stable),
                                                         "future-component", "future-tool",
                                                         "future-os-riscv128", "future-os-riscv128"));
    package_catalog_free(&catalog);
}

static void test_future_stable_does_not_promote_old_release(void) {
    PackageCatalog catalog;
    char path[256];
    char stable[MAX_IDENTIFIER_LEN];
    FILE *file;

    package_catalog_init(&catalog);
    build_path(path, sizeof(path), "future-stable.cfg");
    file = fopen(path, "wb"); TEST_ASSERT_NOT_NULL(file);
    write_header(file, 2);
    write_package(file, 0, "compiler", "clang", "linux-x64", "linux-x64", "23.1.0", 0,
                  NULL, "zip", "https://example.invalid/old.zip", SHA_A);
    write_package(file, 1, "compiler", "clang", "linux-x64", "linux-x64", "24.0.0", 1,
                  NULL, "future-format", "https://example.invalid/future.pkg", SHA_B);
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
    TEST_ASSERT_EQUAL_INT(CUP_OK, package_catalog_load_path(&catalog, path));
    TEST_ASSERT_TRUE(catalog.packages[0].operational);
    TEST_ASSERT_FALSE(catalog.packages[1].operational);
    TEST_ASSERT_EQUAL_INT(CUP_ERR_NOT_AVAILABLE,
                          package_catalog_resolve_stable(&catalog, stable, sizeof(stable),
                                                         "compiler", "clang", "linux-x64", "linux-x64"));
    package_catalog_free(&catalog);
}

static void test_future_catalog_format_is_unsupported_not_corrupt(void) {
    PackageCatalog catalog;
    char path[256];

    package_catalog_init(&catalog);
    build_path(path, sizeof(path), "future-format.cfg");
    write_text(path,
               "format=2\n"
               "revision=1\n"
               "update_url=https://example.invalid/catalog.cfg\n");
    TEST_ASSERT_EQUAL_INT(CUP_ERR_NOT_AVAILABLE, package_catalog_load_path(&catalog, path));
    TEST_ASSERT_EQUAL_size_t(0, catalog.count);

    build_path(path, sizeof(path), "malformed-format-text.cfg");
    write_text(path,
               "format=x\n"
               "revision=1\n"
               "update_url=https://example.invalid/catalog.cfg\n");
    TEST_ASSERT_EQUAL_INT(CUP_ERR_CATALOG, package_catalog_load_path(&catalog, path));

    build_path(path, sizeof(path), "malformed-format-leading-zero.cfg");
    write_text(path,
               "format=01\n"
               "revision=1\n"
               "update_url=https://example.invalid/catalog.cfg\n");
    TEST_ASSERT_EQUAL_INT(CUP_ERR_CATALOG, package_catalog_load_path(&catalog, path));
    package_catalog_free(&catalog);
}

static void test_structural_rejections(void) {
    assert_rejected("missing-revision.cfg",
                    "format=1\nupdate_url=https://example.invalid/catalog.cfg\n");
    assert_rejected("noncanonical-revision.cfg",
                    "format=1\nrevision=01\nupdate_url=https://example.invalid/catalog.cfg\n");
    assert_rejected("bad-update-url.cfg",
                    "format=1\nrevision=1\nupdate_url=http://example.invalid/catalog.cfg\n");
    assert_rejected("unknown-core.cfg",
                    "format=1\nrevision=1\nupdate_url=https://example.invalid/catalog.cfg\nfoo=bar\n");
    assert_rejected("meta.cfg",
                    "format=1\nrevision=1\nupdate_url=https://example.invalid/catalog.cfg\nmeta.generator=test\n");
    assert_rejected("manifest.cfg",
                    "format=1\nrevision=1\nupdate_url=https://example.invalid/catalog.cfg\n"
                    "package.0.component=compiler\npackage.0.tool=clang\npackage.0.host=linux-x64\n"
                    "package.0.target=linux-x64\npackage.0.version=23.1.0\npackage.0.stable=true\n"
                    "package.0.manifest_sha256=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\n"
                    "package.0.artifact.0.format=zip\npackage.0.artifact.0.url=https://example.invalid/a.zip\n"
                    "package.0.artifact.0.sha256=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\n");
    assert_rejected("incomplete-artifact.cfg",
                    "format=1\nrevision=1\nupdate_url=https://example.invalid/catalog.cfg\n"
                    "package.0.component=compiler\npackage.0.tool=clang\npackage.0.host=linux-x64\n"
                    "package.0.target=linux-x64\npackage.0.version=23.1.0\npackage.0.stable=true\n"
                    "package.0.artifact.0.format=zip\npackage.0.artifact.0.url=https://example.invalid/a.zip\n");
}

static void test_duplicate_and_semantic_stable_rejected(void) {
    PackageCatalog catalog;
    char path[256];
    FILE *file;

    package_catalog_init(&catalog);
    build_path(path, sizeof(path), "duplicate.cfg");
    file = fopen(path, "wb"); TEST_ASSERT_NOT_NULL(file);
    write_header(file, 1);
    write_package(file, 0, "compiler", "clang", "linux-x64", "linux-x64", "23.1.0", 0,
                  NULL, "zip", "https://example.invalid/a.zip", SHA_A);
    write_package(file, 1, "compiler", "clang", "linux-x64", "linux-x64", "23.1.0", 1,
                  NULL, "zip", "https://example.invalid/b.zip", SHA_B);
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_CATALOG, package_catalog_load_path(&catalog, path));

    build_path(path, sizeof(path), "wrong-stable.cfg");
    file = fopen(path, "wb"); TEST_ASSERT_NOT_NULL(file);
    write_header(file, 1);
    write_package(file, 0, "compiler", "clang", "linux-x64", "linux-x64", "23.1.0", 1,
                  NULL, "zip", "https://example.invalid/a.zip", SHA_A);
    write_package(file, 1, "compiler", "clang", "linux-x64", "linux-x64", "24.0.0", 0,
                  NULL, "zip", "https://example.invalid/b.zip", SHA_B);
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_CATALOG, package_catalog_load_path(&catalog, path));
    package_catalog_free(&catalog);
}

static void test_dynamic_artifact_storage(void) {
    PackageCatalog catalog;
    char path[256];
    FILE *file;
    size_t i;

    package_catalog_init(&catalog);
    build_path(path, sizeof(path), "many-artifacts.cfg");
    file = fopen(path, "wb"); TEST_ASSERT_NOT_NULL(file);
    write_header(file, 1);
    TEST_ASSERT_TRUE(fprintf(file,
                             "package.0.component=compiler\npackage.0.tool=clang\n"
                             "package.0.host=linux-x64\npackage.0.target=linux-x64\n"
                             "package.0.version=23.1.0\npackage.0.stable=true\n") > 0);
    for (i = 0; i < 20; ++i) {
        TEST_ASSERT_TRUE(fprintf(file,
                                 "package.0.artifact.%zu.format=future%zu\n"
                                 "package.0.artifact.%zu.url=https://example.invalid/%zu.pkg\n"
                                 "package.0.artifact.%zu.sha256=%s\n",
                                 i, i, i, i, i, SHA_A) > 0);
    }
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
    TEST_ASSERT_EQUAL_INT(CUP_OK, package_catalog_load_path(&catalog, path));
    TEST_ASSERT_EQUAL_size_t(20, catalog.packages[0].artifact_count);
    TEST_ASSERT_FALSE(catalog.packages[0].operational);
    package_catalog_free(&catalog);
}

static void test_query_errors_and_source_choice(void) {
    PackageCatalog catalog;
    char path[256];
    char value[MAX_CATALOG_URL_LEN];
    char digest[65];
    char cwd[MAX_PATH_LEN];
    char config_path[MAX_PATH_LEN];
    char development_path[MAX_PATH_LEN];
    int flag = 1;

    package_catalog_init(&catalog);
    build_path(path, sizeof(path), "installed.cfg");
    write_valid_catalog(path);
    TEST_ASSERT_EQUAL_INT(CUP_OK, package_catalog_load_path(&catalog, path));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_BUFFER_TOO_SMALL,
                          package_catalog_resolve_artifact(&catalog, "compiler", "clang", "linux-x64",
                                                           "linux-x64", "24.0.0-rev1", "zip", value, 4,
                                                           digest, sizeof(digest)));
    TEST_ASSERT_EQUAL_STRING("", value);
    TEST_ASSERT_EQUAL_STRING("", digest);
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          package_catalog_has_package(&catalog, "compiler", "clang", "linux-x64",
                                                      "linux-x64", NULL));
    package_catalog_free(&catalog);

    TEST_ASSERT_EQUAL_INT(CUP_OK, text_copy(installed_path, sizeof(installed_path), path));
    installed_exists = 1;
    TEST_ASSERT_EQUAL_INT(CUP_OK, package_catalog_load(&catalog));
    package_catalog_free(&catalog);

    TEST_ASSERT_NOT_NULL(getcwd(cwd, sizeof(cwd)));
    TEST_ASSERT_TRUE(snprintf(config_path, sizeof(config_path), "%s/config", temp_dir) > 0);
    if (test_mkdir(config_path, 0755) != 0) TEST_ASSERT_EQUAL_INT(EEXIST, errno);
    TEST_ASSERT_TRUE(snprintf(development_path, sizeof(development_path), "%s/catalog.cfg", config_path) > 0);
    write_valid_catalog(development_path);
    TEST_ASSERT_EQUAL_INT(0, chdir(temp_dir));
    installed_exists = 0; development_exists = 1;
    TEST_ASSERT_EQUAL_INT(CUP_OK, package_catalog_load(&catalog));
    package_catalog_free(&catalog);
    TEST_ASSERT_EQUAL_INT(0, chdir(cwd));

    installed_exists = 0; development_exists = 0;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_CATALOG, package_catalog_load(&catalog));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, package_catalog_load(NULL));
    (void)flag;
    package_catalog_free(&catalog);
}

static void test_load_failures(void) {
    PackageCatalog catalog;
    char path[256];
    char long_path[MAX_PATH_LEN + 8];
    package_catalog_init(&catalog);
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, package_catalog_load_path(NULL, "x"));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, package_catalog_load_path(&catalog, ""));
    build_path(path, sizeof(path), "missing.cfg");
    TEST_ASSERT_EQUAL_INT(CUP_ERR_CATALOG, package_catalog_load_path(&catalog, path));
    memset(long_path, 'p', sizeof(long_path) - 1); long_path[sizeof(long_path) - 1] = '\0';
    TEST_ASSERT_EQUAL_INT(CUP_ERR_BUFFER_TOO_SMALL, package_catalog_load_path(&catalog, long_path));
    package_catalog_free(&catalog);
}

int main(void) {
    TEST_ASSERT_NOT_NULL(test_make_temp_directory(temp_dir, sizeof(temp_dir), "cup-catalog-test"));
    UNITY_BEGIN();
    RUN_TEST(test_load_queries_and_artifacts);
    RUN_TEST(test_future_records_are_structural_not_operational);
    RUN_TEST(test_future_stable_does_not_promote_old_release);
    RUN_TEST(test_future_catalog_format_is_unsupported_not_corrupt);
    RUN_TEST(test_structural_rejections);
    RUN_TEST(test_duplicate_and_semantic_stable_rejected);
    RUN_TEST(test_dynamic_artifact_storage);
    RUN_TEST(test_query_errors_and_source_choice);
    RUN_TEST(test_load_failures);
    return UNITY_END();
}
