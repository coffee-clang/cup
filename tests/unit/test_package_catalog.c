/*
 * Exercises catalog tuple assembly, source choice, strict validation, catalog
 * queries and HTTPS template expansion.
 */

#include "package_catalog.h"
#include "registry.h"
#include "layout.h"
#include "system.h"
#include "text.h"
#include "unity.h"
#include "test_platform.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Scenario controls and observations. Configured results drive the boundary doubles below;
 * counters record the calls made by production code.
 */

static char temp_dir[CUP_TEST_TEMP_PATH_SIZE];
static char installed_path[MAX_PATH_LEN];
static CupError layout_error;
static CupError exists_error;
static int installed_exists;
static int development_exists;
static int exists_calls;
static int exists_error_call;

/* Fixture lifecycle and local construction helpers. */

void setUp(void) {
    layout_error = CUP_OK;
    exists_error = CUP_OK;
    installed_exists = 0;
    development_exists = 0;
    exists_calls = 0;
    exists_error_call = 0;
    installed_path[0] = '\0';
}

void tearDown(void) {
}

/*
 * Controlled boundary doubles. Each implementation exposes one dependency through the scenario
 * state above.
 */

int download_insecure_loopback_is_allowed(const char *url) {
    (void)url;
    return 0;
}

CupError checksum_sha256_bytes(const unsigned char *data,
                               size_t data_size,
                               char *hex,
                               size_t size) {
    (void)data;
    (void)data_size;
    if (hex == NULL || size < 65) {
        return CUP_ERR_BUFFER_TOO_SMALL;
    }
    memset(hex, 'a', 64);
    hex[64] = '\0';
    return CUP_OK;
}

CupError layout_get_package_catalog_path(char *buffer, size_t size) {
    if (layout_error != CUP_OK) {
        return layout_error;
    }
    return text_copy(buffer, size, installed_path);
}

CupError system_path_exists(const char *path, int *exists) {
    exists_calls++;
    if (exists_error != CUP_OK || exists_calls == exists_error_call) {
        return exists_error != CUP_OK ? exists_error : CUP_ERR_FILESYSTEM;
    }
    if (path == NULL || exists == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    *exists = strcmp(path, installed_path) == 0 ? installed_exists : development_exists;
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

static void write_tuple(FILE *file,
                        const char *component,
                        const char *tool,
                        const char *host,
                        const char *target,
                        const char *stable,
                        const char *versions,
                        const char *format,
                        const char *formats,
                        const char *url,
                        const char *checksum) {
    TEST_ASSERT_TRUE(fprintf(file,
                             "%s.%s.%s.%s.stable_version=%s\n"
                             "%s.%s.%s.%s.available_versions=%s\n"
                             "%s.%s.%s.%s.default_format=%s\n"
                             "%s.%s.%s.%s.formats=%s\n"
                             "%s.%s.%s.%s.url_template=%s\n"
                             "%s.%s.%s.%s.checksum_url_template=%s\n",
                             component,
                             tool,
                             host,
                             target,
                             stable,
                             component,
                             tool,
                             host,
                             target,
                             versions,
                             component,
                             tool,
                             host,
                             target,
                             format,
                             component,
                             tool,
                             host,
                             target,
                             formats,
                             component,
                             tool,
                             host,
                             target,
                             url,
                             component,
                             tool,
                             host,
                             target,
                             checksum) > 0);
}

static void write_valid_catalog(const char *path) {
    FILE *file = fopen(path, "wb");

    TEST_ASSERT_NOT_NULL(file);
    TEST_ASSERT_TRUE(fprintf(file, "format=1\n") > 0);
    write_tuple(file,
                "compiler",
                "clang",
                "linux-x64",
                "linux-x64",
                "22.1.5",
                "22.1.5,21.1.5",
                "tar.xz",
                "tar.xz,tar.gz,zip",
                "https://example.invalid/{tool}-{version}-{host_platform}-"
                "{target_platform}.{format}",
                "https://example.invalid/{tool}-{version}-{host_platform}-"
                "{target_platform}/SHA256SUMS");
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
}

static void assert_rejected_format_marker(const char *name, const char *marker) {
    PackageCatalog catalog;
    char path[256];
    FILE *file;

    build_path(path, sizeof(path), name);
    file = fopen(path, "wb");
    TEST_ASSERT_NOT_NULL(file);
    TEST_ASSERT_TRUE(fprintf(file, "%s\n", marker) > 0);
    write_tuple(file,
                "compiler",
                "clang",
                "linux-x64",
                "linux-x64",
                "22.1.5",
                "22.1.5",
                "tar.xz",
                "tar.xz",
                "https://example.invalid/{version}-{host_platform}-{target_platform}.{format}",
                "https://example.invalid/{version}-{host_platform}-{target_platform}/SHA256SUMS");
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
    package_catalog_init(&catalog);
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_CATALOG,
        package_catalog_load_path(&catalog, path));
    package_catalog_free(&catalog);
}

static void assert_rejected_raw(const char *name, const char *body) {
    PackageCatalog catalog;
    char path[256];

    package_catalog_init(&catalog);
    build_path(path, sizeof(path), name);
    write_text(path, body);
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_CATALOG,
        package_catalog_load_path(&catalog, path));
    TEST_ASSERT_EQUAL_size_t(0, catalog.count);
    package_catalog_free(&catalog);
}

static void assert_rejected(const char *name, const char *body) {
    char catalog[8192];

    TEST_ASSERT_TRUE(snprintf(catalog, sizeof(catalog), "format=1\n%s", body) > 0);
    assert_rejected_raw(name, catalog);
}

/*
 * Test cases exercise the real production entry point while changing only controlled boundary
 * outcomes.
 */

static void test_load_queries(void) {
    PackageCatalog catalog;
    char path[256];
    char value[MAX_CATALOG_URL_LEN];
    int flag = 0;

    package_catalog_init(&catalog);
    build_path(path, sizeof(path), "valid.cfg");
    write_valid_catalog(path);
    TEST_ASSERT_EQUAL_INT(
        CUP_OK, package_catalog_load_path(&catalog, path));
    TEST_ASSERT_EQUAL_size_t(1, catalog.count);

    TEST_ASSERT_EQUAL_INT(
        CUP_OK,
        package_catalog_resolve_stable(
            &catalog, value, sizeof(value), "compiler", "clang", "linux-x64", "linux-x64"));
    TEST_ASSERT_EQUAL_STRING("22.1.5", value);
    TEST_ASSERT_EQUAL_INT(
        CUP_OK,
        package_catalog_get_default_format(
            &catalog, value, sizeof(value), "compiler", "clang", "linux-x64", "linux-x64"));
    TEST_ASSERT_EQUAL_STRING("tar.xz", value);

    TEST_ASSERT_EQUAL_INT(
        CUP_OK,
        package_catalog_is_stable(
            &catalog, "compiler", "clang", "linux-x64", "linux-x64", "22.1.5", &flag));
    TEST_ASSERT_TRUE(flag);
    TEST_ASSERT_EQUAL_INT(
        CUP_OK,
        package_catalog_is_stable(
            &catalog, "compiler", "clang", "linux-x64", "linux-x64", "21.1.5", &flag));
    TEST_ASSERT_FALSE(flag);
    TEST_ASSERT_EQUAL_INT(CUP_OK,
                          package_catalog_has_package(
                              &catalog, "compiler", "clang", "linux-x64", "linux-x64", &flag));
    TEST_ASSERT_TRUE(flag);
    TEST_ASSERT_EQUAL_INT(
        CUP_OK,
        package_catalog_has_package(&catalog, "linker", "ld", "linux-x64", "linux-x64", &flag));
    TEST_ASSERT_FALSE(flag);
    TEST_ASSERT_EQUAL_INT(
        CUP_OK,
        package_catalog_has_version(
            &catalog, "compiler", "clang", "linux-x64", "linux-x64", "21.1.5", &flag));
    TEST_ASSERT_TRUE(flag);
    TEST_ASSERT_EQUAL_INT(
        CUP_OK,
        package_catalog_has_version(
            &catalog, "compiler", "clang", "linux-x64", "linux-x64", "20.1.0", &flag));
    TEST_ASSERT_FALSE(flag);
    TEST_ASSERT_EQUAL_INT(
        CUP_OK,
        package_catalog_has_format(
            &catalog, "compiler", "clang", "linux-x64", "linux-x64", "zip", &flag));
    TEST_ASSERT_TRUE(flag);
    TEST_ASSERT_EQUAL_INT(
        CUP_OK,
        package_catalog_has_format(
            &catalog, "compiler", "clang", "linux-x64", "linux-x64", "7z", &flag));
    TEST_ASSERT_FALSE(flag);

    TEST_ASSERT_EQUAL_INT(CUP_OK,
                          package_catalog_build_url(&catalog,
                                                    value,
                                                    sizeof(value),
                                                    "compiler",
                                                    "clang",
                                                    "linux-x64",
                                                    "linux-x64",
                                                    "22.1.5",
                                                    "tar.xz"));
    TEST_ASSERT_EQUAL_STRING("https://example.invalid/clang-22.1.5-linux-x64-linux-x64.tar.xz",
                             value);
    TEST_ASSERT_EQUAL_INT(CUP_OK,
                          package_catalog_build_checksum_url(&catalog,
                                                             value,
                                                             sizeof(value),
                                                             "compiler",
                                                             "clang",
                                                             "linux-x64",
                                                             "linux-x64",
                                                             "22.1.5"));
    TEST_ASSERT_EQUAL_STRING("https://example.invalid/clang-22.1.5-linux-x64-linux-x64/SHA256SUMS",
                             value);
    package_catalog_free(&catalog);
}

static void test_source_choice(void) {
    PackageCatalog catalog;
    char path[256];

    package_catalog_init(&catalog);
    build_path(path, sizeof(path), "installed.cfg");
    write_valid_catalog(path);
    TEST_ASSERT_EQUAL_INT(CUP_OK, text_copy(installed_path, sizeof(installed_path), path));

    installed_exists = 1;
    TEST_ASSERT_EQUAL_INT(CUP_OK, package_catalog_load(&catalog));
    package_catalog_free(&catalog);

    TEST_ASSERT_EQUAL_INT(CUP_OK, text_copy(installed_path, sizeof(installed_path), path));
    TEST_ASSERT_EQUAL_INT(CUP_OK, package_catalog_load_installed(&catalog));
    package_catalog_free(&catalog);

    {
        char cwd[MAX_PATH_LEN];
        char config_path[MAX_PATH_LEN];
        char development_path[MAX_PATH_LEN];

        TEST_ASSERT_NOT_NULL(getcwd(cwd, sizeof(cwd)));
        TEST_ASSERT_TRUE(snprintf(config_path, sizeof(config_path), "%s/config", temp_dir) > 0);
        if (test_mkdir(config_path, 0755) != 0) {
            TEST_ASSERT_EQUAL_INT(EEXIST, errno);
        }
        TEST_ASSERT_TRUE(
            snprintf(development_path, sizeof(development_path), "%s/packages.cfg", config_path) >
            0);
        write_valid_catalog(development_path);
        TEST_ASSERT_EQUAL_INT(0, chdir(temp_dir));
        TEST_ASSERT_EQUAL_INT(CUP_OK, text_copy(installed_path, sizeof(installed_path), path));
        installed_exists = 0;
        development_exists = 1;
        TEST_ASSERT_EQUAL_INT(CUP_ERR_CATALOG, package_catalog_load(&catalog));
        TEST_ASSERT_EQUAL_INT(CUP_OK, package_catalog_load_development(&catalog));
        package_catalog_free(&catalog);
        TEST_ASSERT_EQUAL_INT(0, chdir(cwd));
    }

    layout_error = CUP_ERR_FILESYSTEM;
    TEST_ASSERT_EQUAL_INT(CUP_OK,
                          package_catalog_load_path(&catalog, path));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM, package_catalog_load(&catalog));
    TEST_ASSERT_EQUAL_size_t(0, catalog.count);
    TEST_ASSERT_EQUAL_INT(CUP_OK,
                          package_catalog_load_path(&catalog, path));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM, package_catalog_load_installed(&catalog));
    TEST_ASSERT_EQUAL_size_t(0, catalog.count);
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, package_catalog_load_installed(NULL));
    layout_error = CUP_OK;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, package_catalog_load(NULL));

    exists_error = CUP_ERR_FILESYSTEM;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM, package_catalog_load(&catalog));
    exists_error = CUP_OK;
    installed_exists = 0;
    development_exists = 0;
    exists_calls = 0;
    exists_error_call = 0;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_CATALOG, package_catalog_load(&catalog));
    package_catalog_free(&catalog);
}

static void test_tuple_growth(void) {
    static const char *const platforms[] = {
        "linux-x64", "linux-arm64", "windows-x64", "macos-x64", "macos-arm64"};
    PackageCatalog catalog;
    char path[256];
    FILE *file;
    size_t i;

    package_catalog_init(&catalog);
    build_path(path, sizeof(path), "many.cfg");
    file = fopen(path, "wb");
    TEST_ASSERT_NOT_NULL(file);
    TEST_ASSERT_TRUE(fprintf(file, "format=1\n") > 0);
    for (i = 0; i < 17; ++i) {
        write_tuple(file,
                    "compiler",
                    "clang",
                    platforms[i / 5],
                    platforms[i % 5],
                    "22.1.5",
                    "22.1.5",
                    "tar.xz",
                    "tar.xz",
                    "https://example.invalid/{version}-{host_platform}-"
                    "{target_platform}.{format}",
                    "https://example.invalid/{version}-{host_platform}-"
                    "{target_platform}/SHA256SUMS");
    }
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
    TEST_ASSERT_EQUAL_INT(
        CUP_OK, package_catalog_load_path(&catalog, path));
    TEST_ASSERT_EQUAL_size_t(17, catalog.count);
    TEST_ASSERT_TRUE(catalog.capacity >= 17);
    package_catalog_free(&catalog);
}

static void test_record_errors(void) {
    assert_rejected_raw("missing-format.cfg",
                        "compiler.clang.linux-x64.linux-x64.stable_version=22.1.5\n");
    assert_rejected_raw("wrong-format.cfg", "format=2\n");
    assert_rejected_format_marker("leading-comment-format.cfg", "# comment\nformat=1");
    assert_rejected_format_marker("spaced-format.cfg", "format = 1");
    assert_rejected("empty.cfg", "# only comments\n\n");
    assert_rejected("no-equals.cfg", "compiler.clang.linux-x64.linux-x64.stable_version\n");
    assert_rejected("short-key.cfg", "compiler.clang.linux-x64.stable_version=1\n");
    assert_rejected("unknown-field.cfg", "compiler.clang.linux-x64.linux-x64.unknown=value\n");
    assert_rejected("bad-component.cfg", "unknown.clang.linux-x64.linux-x64.stable_version=1\n");
    assert_rejected("bad-tool.cfg", "compiler.gdb.linux-x64.linux-x64.stable_version=1\n");
    assert_rejected("bad-platform.cfg", "compiler.clang.plan9-x64.linux-x64.stable_version=1\n");
    assert_rejected("bad-target.cfg", "compiler.clang.linux-x64.plan9-x64.stable_version=1\n");
    assert_rejected("duplicate.cfg",
                    "compiler.clang.linux-x64.linux-x64.stable_version=22.1.5\n"
                    "compiler.clang.linux-x64.linux-x64.stable_version=21.1.5\n");
}

static void test_value_errors(void) {
    const char *head = "compiler.clang.linux-x64.linux-x64.stable_version=22.1.5\n"
                       "compiler.clang.linux-x64.linux-x64.available_versions=22.1.5\n"
                       "compiler.clang.linux-x64.linux-x64.default_format=tar.xz\n"
                       "compiler.clang.linux-x64.linux-x64.formats=tar.xz\n";
    char body[4096];

    TEST_ASSERT_TRUE(
        snprintf(body,
                 sizeof(body),
                 "%s%s",
                 head,
                 "compiler.clang.linux-x64.linux-x64.url_template=https://example.invalid/"
                 "{version}-{host_platform}-{target_platform}.{format}\n") > 0);
    assert_rejected("missing-field.cfg", body);

    TEST_ASSERT_TRUE(snprintf(body,
                              sizeof(body),
                              "compiler.clang.linux-x64.linux-x64.stable_version=22.1.5\n"
                              "compiler.clang.linux-x64.linux-x64.available_versions=21.1.5\n"
                              "compiler.clang.linux-x64.linux-x64.default_format=tar.xz\n"
                              "compiler.clang.linux-x64.linux-x64.formats=tar.xz\n"
                              "compiler.clang.linux-x64.linux-x64.url_template=https://x/"
                              "{version}-{host_platform}-{target_platform}.{format}\n"
                              "compiler.clang.linux-x64.linux-x64.checksum_url_template=https://x/"
                              "{version}-{host_platform}-{target_platform}\n") > 0);
    assert_rejected("stable-missing.cfg", body);

    TEST_ASSERT_TRUE(snprintf(body,
                              sizeof(body),
                              "compiler.clang.linux-x64.linux-x64.stable_version=bad version\n"
                              "compiler.clang.linux-x64.linux-x64.available_versions=bad-version\n"
                              "compiler.clang.linux-x64.linux-x64.default_format=tar.xz\n"
                              "compiler.clang.linux-x64.linux-x64.formats=tar.xz\n"
                              "compiler.clang.linux-x64.linux-x64.url_template=https://x/"
                              "{version}-{host_platform}-{target_platform}.{format}\n"
                              "compiler.clang.linux-x64.linux-x64.checksum_url_template=https://x/"
                              "{version}-{host_platform}-{target_platform}\n") > 0);
    assert_rejected("bad-identifier.cfg", body);

    TEST_ASSERT_TRUE(snprintf(body,
                              sizeof(body),
                              "compiler.clang.linux-x64.linux-x64.stable_version=stable\n"
                              "compiler.clang.linux-x64.linux-x64.available_versions=stable\n"
                              "compiler.clang.linux-x64.linux-x64.default_format=tar.xz\n"
                              "compiler.clang.linux-x64.linux-x64.formats=tar.xz\n"
                              "compiler.clang.linux-x64.linux-x64.url_template=https://x/"
                              "{version}-{host_platform}-{target_platform}.{format}\n"
                              "compiler.clang.linux-x64.linux-x64.checksum_url_template=https://x/"
                              "{version}-{host_platform}-{target_platform}\n") > 0);
    assert_rejected("symbolic-concrete-version.cfg", body);

    TEST_ASSERT_TRUE(snprintf(body,
                              sizeof(body),
                              "compiler.clang.linux-x64.linux-x64.stable_version=22.1.5\n"
                              "compiler.clang.linux-x64.linux-x64.available_versions=22.1.5,22.1.5-RC1\n"
                              "compiler.clang.linux-x64.linux-x64.default_format=tar.xz\n"
                              "compiler.clang.linux-x64.linux-x64.formats=tar.xz\n"
                              "compiler.clang.linux-x64.linux-x64.url_template=https://x/"
                              "{version}-{host_platform}-{target_platform}.{format}\n"
                              "compiler.clang.linux-x64.linux-x64.checksum_url_template=https://x/"
                              "{version}-{host_platform}-{target_platform}\n") > 0);
    assert_rejected("noncanonical-version.cfg", body);

    TEST_ASSERT_TRUE(snprintf(body,
                              sizeof(body),
                              "compiler.clang.linux-x64.linux-x64.stable_version=22.1.5\n"
                              "compiler.clang.linux-x64.linux-x64.available_versions=22.1.5\n"
                              "compiler.clang.linux-x64.linux-x64.default_format=bad format\n"
                              "compiler.clang.linux-x64.linux-x64.formats=bad-format\n"
                              "compiler.clang.linux-x64.linux-x64.url_template=https://x/"
                              "{version}-{host_platform}-{target_platform}.{format}\n"
                              "compiler.clang.linux-x64.linux-x64.checksum_url_template=https://x/"
                              "{version}-{host_platform}-{target_platform}\n") > 0);
    assert_rejected("bad-format-id.cfg", body);

    TEST_ASSERT_TRUE(snprintf(body,
                              sizeof(body),
                              "compiler.clang.linux-x64.linux-x64.stable_version=22.1.5\n"
                              "compiler.clang.linux-x64.linux-x64.available_versions=22.1.5\n"
                              "compiler.clang.linux-x64.linux-x64.default_format=zip\n"
                              "compiler.clang.linux-x64.linux-x64.formats=tar.xz\n"
                              "compiler.clang.linux-x64.linux-x64.url_template=https://x/"
                              "{version}-{host_platform}-{target_platform}.{format}\n"
                              "compiler.clang.linux-x64.linux-x64.checksum_url_template=https://x/"
                              "{version}-{host_platform}-{target_platform}\n") > 0);
    assert_rejected("format-missing.cfg", body);

    TEST_ASSERT_TRUE(
        snprintf(body,
                 sizeof(body),
                 "compiler.clang.linux-x64.linux-x64.stable_version=22.1.5\n"
                 "compiler.clang.linux-x64.linux-x64.available_versions=22.1.5,bad/value\n"
                 "compiler.clang.linux-x64.linux-x64.default_format=tar.xz\n"
                 "compiler.clang.linux-x64.linux-x64.formats=tar.xz\n"
                 "compiler.clang.linux-x64.linux-x64.url_template=https://x/"
                 "{version}-{host_platform}-{target_platform}.{format}\n"
                 "compiler.clang.linux-x64.linux-x64.checksum_url_template=https://x/"
                 "{version}-{host_platform}-{target_platform}\n") > 0);
    assert_rejected("bad-list.cfg", body);

    TEST_ASSERT_TRUE(
        snprintf(body,
                 sizeof(body),
                 "compiler.clang.linux-x64.linux-x64.stable_version=22.1.5\n"
                 "compiler.clang.linux-x64.linux-x64.available_versions=22.1.5,22.1.5\n"
                 "compiler.clang.linux-x64.linux-x64.default_format=zip\n"
                 "compiler.clang.linux-x64.linux-x64.formats=tar.xz,tar.xz\n"
                 "compiler.clang.linux-x64.linux-x64.url_template=https://x/"
                 "{version}-{host_platform}-{target_platform}.{format}\n"
                 "compiler.clang.linux-x64.linux-x64.checksum_url_template=https://x/"
                 "{version}-{host_platform}-{target_platform}\n") > 0);
    assert_rejected("duplicate-list.cfg", body);
}

static void test_template_errors(void) {
    const char *versions = "22.1.5";
    const char *formats = "tar.xz";
    const char *valid_checksum = "https://x/{version}-{host_platform}-{target_platform}";
    char path[256];
    FILE *file;
    PackageCatalog catalog;

#define WRITE_BAD(name, url, checksum) \
    do { \
        build_path(path, sizeof(path), (name)); \
        file = fopen(path, "wb"); \
        TEST_ASSERT_NOT_NULL(file); \
        TEST_ASSERT_TRUE(fprintf(file, "format=1\n") > 0); \
        write_tuple(file, \
                    "compiler", \
                    "clang", \
                    "linux-x64", \
                    "linux-x64", \
                    "22.1.5", \
                    versions, \
                    "tar.xz", \
                    formats, \
                    (url), \
                    (checksum)); \
        TEST_ASSERT_EQUAL_INT(0, fclose(file)); \
        package_catalog_init(&catalog); \
        TEST_ASSERT_EQUAL_INT( \
            CUP_ERR_CATALOG, \
            package_catalog_load_path(&catalog, path)); \
        package_catalog_free(&catalog); \
    } while (0)

    WRITE_BAD("http.cfg",
              "http://x/{version}-{host_platform}-{target_platform}.{format}",
              valid_checksum);
    WRITE_BAD("space.cfg",
              "https://x/{version}- {host_platform}-{target_platform}.{format}",
              valid_checksum);
    WRITE_BAD("close.cfg",
              "https://x/}-{version}-{host_platform}-{target_platform}.{format}",
              valid_checksum);
    WRITE_BAD("open.cfg",
              "https://x/{bad-{version}-{host_platform}-{target_platform}.{format}",
              valid_checksum);
    WRITE_BAD("unknown.cfg",
              "https://x/{unknown}-{version}-{host_platform}-{target_platform}.{format}",
              valid_checksum);
    WRITE_BAD("unclosed.cfg", "https://x/{version", valid_checksum);
    WRITE_BAD("missing.cfg", "https://x/{version}-{host_platform}.{format}", valid_checksum);
    WRITE_BAD("checksum-format.cfg",
              "https://x/{version}-{host_platform}-{target_platform}.{format}",
              "https://x/{version}-{host_platform}-{target_platform}.{format}");
#undef WRITE_BAD
}

static void assert_invalid_catalog_load_and_resolution(PackageCatalog *catalog, char *value) {    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_INVALID_INPUT,
        package_catalog_resolve_stable(
            catalog, NULL, MAX_CATALOG_URL_LEN, "compiler", "clang", "linux-x64", "linux-x64"));
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_INVALID_INPUT,
        package_catalog_resolve_stable(
            NULL, value, MAX_CATALOG_URL_LEN, "compiler", "clang", "linux-x64", "linux-x64"));
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_INVALID_INPUT,
        package_catalog_resolve_stable(
            catalog, value, MAX_CATALOG_URL_LEN, "", "clang", "linux-x64", "linux-x64"));
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_INVALID_INPUT,
        package_catalog_resolve_stable(
            catalog, value, MAX_CATALOG_URL_LEN, "compiler", "", "linux-x64", "linux-x64"));
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_INVALID_INPUT,
        package_catalog_resolve_stable(
            catalog, value, MAX_CATALOG_URL_LEN, "compiler", "clang", "", "linux-x64"));
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_INVALID_INPUT,
        package_catalog_resolve_stable(
            catalog, value, MAX_CATALOG_URL_LEN, "compiler", "clang", "linux-x64", ""));
}

static void assert_invalid_catalog_stability(PackageCatalog *catalog) {
    int flag = 1;
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_INVALID_INPUT,
        package_catalog_is_stable(
            catalog, "compiler", "clang", "linux-x64", "linux-x64", "22.1.5", NULL));
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_INVALID_INPUT,
        package_catalog_is_stable(
            catalog, "compiler", "clang", "linux-x64", "linux-x64", NULL, &flag));
    TEST_ASSERT_FALSE(flag);
    flag = 1;
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_INVALID_INPUT,
        package_catalog_is_stable(
            NULL, "compiler", "clang", "linux-x64", "linux-x64", "22.1.5", &flag));
    TEST_ASSERT_FALSE(flag);
    flag = 1;
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_INVALID_INPUT,
        package_catalog_is_stable(
            catalog, "", "clang", "linux-x64", "linux-x64", "22.1.5", &flag));
    TEST_ASSERT_FALSE(flag);
    flag = 1;
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_INVALID_INPUT,
        package_catalog_is_stable(
            catalog, "compiler", "", "linux-x64", "linux-x64", "22.1.5", &flag));
    TEST_ASSERT_FALSE(flag);
    flag = 1;
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_INVALID_INPUT,
        package_catalog_is_stable(
            catalog, "compiler", "clang", "", "linux-x64", "22.1.5", &flag));
    TEST_ASSERT_FALSE(flag);
    flag = 1;
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_INVALID_INPUT,
        package_catalog_is_stable(
            catalog, "compiler", "clang", "linux-x64", "", "22.1.5", &flag));
    TEST_ASSERT_FALSE(flag);
}

static void assert_invalid_catalog_package_presence(PackageCatalog *catalog) {
    int flag = 1;
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_INVALID_INPUT,
        package_catalog_has_package(
            catalog, "compiler", "clang", "linux-x64", "linux-x64", NULL));
    flag = 1;
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_INVALID_INPUT,
        package_catalog_has_package(
            NULL, "compiler", "clang", "linux-x64", "linux-x64", &flag));
    TEST_ASSERT_FALSE(flag);
    flag = 1;
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_INVALID_INPUT,
        package_catalog_has_package(catalog, "", "clang", "linux-x64", "linux-x64", &flag));
    TEST_ASSERT_FALSE(flag);
    flag = 1;
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_INVALID_INPUT,
        package_catalog_has_package(catalog, "compiler", "", "linux-x64", "linux-x64", &flag));
    TEST_ASSERT_FALSE(flag);
    flag = 1;
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_INVALID_INPUT,
        package_catalog_has_package(catalog, "compiler", "clang", "", "linux-x64", &flag));
    TEST_ASSERT_FALSE(flag);
    flag = 1;
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_INVALID_INPUT,
        package_catalog_has_package(catalog, "compiler", "clang", "linux-x64", "", &flag));
    TEST_ASSERT_FALSE(flag);
}

static void assert_invalid_catalog_version_presence(PackageCatalog *catalog) {
    int flag = 1;
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_INVALID_INPUT,
        package_catalog_has_version(
            catalog, "compiler", "clang", "linux-x64", "linux-x64", "22.1.5", NULL));
    flag = 1;
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_INVALID_INPUT,
        package_catalog_has_version(
            NULL, "compiler", "clang", "linux-x64", "linux-x64", "22.1.5", &flag));
    TEST_ASSERT_FALSE(flag);
    flag = 1;
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_INVALID_INPUT,
        package_catalog_has_version(
            catalog, "", "clang", "linux-x64", "linux-x64", "22.1.5", &flag));
    TEST_ASSERT_FALSE(flag);
    flag = 1;
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_INVALID_INPUT,
        package_catalog_has_version(
            catalog, "compiler", "", "linux-x64", "linux-x64", "22.1.5", &flag));
    TEST_ASSERT_FALSE(flag);
    flag = 1;
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_INVALID_INPUT,
        package_catalog_has_version(
            catalog, "compiler", "clang", "", "linux-x64", "22.1.5", &flag));
    TEST_ASSERT_FALSE(flag);
    flag = 1;
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_INVALID_INPUT,
        package_catalog_has_version(
            catalog, "compiler", "clang", "linux-x64", "", "22.1.5", &flag));
    TEST_ASSERT_FALSE(flag);
    flag = 1;
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_INVALID_INPUT,
        package_catalog_has_version(
            catalog, "compiler", "clang", "linux-x64", "linux-x64", "", &flag));
    TEST_ASSERT_FALSE(flag);
}

static void assert_invalid_catalog_format_presence(PackageCatalog *catalog) {
    int flag = 1;
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_INVALID_INPUT,
        package_catalog_has_format(
            catalog, "compiler", "clang", "linux-x64", "linux-x64", NULL, &flag));
    TEST_ASSERT_FALSE(flag);
    flag = 1;
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_INVALID_INPUT,
        package_catalog_has_format(
            catalog, "compiler", "clang", "linux-x64", "linux-x64", "tar.xz", NULL));
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_INVALID_INPUT,
        package_catalog_has_format(
            NULL, "compiler", "clang", "linux-x64", "linux-x64", "tar.xz", &flag));
    TEST_ASSERT_FALSE(flag);
}

static void assert_invalid_catalog_default_format(PackageCatalog *catalog, char *value) {
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_INVALID_INPUT,
        package_catalog_get_default_format(
            catalog, value, 0, "compiler", "clang", "linux-x64", "linux-x64"));
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_INVALID_INPUT,
        package_catalog_get_default_format(
            catalog, NULL, MAX_CATALOG_URL_LEN, "compiler", "clang", "linux-x64", "linux-x64"));
}

static void assert_invalid_catalog_urls(PackageCatalog *catalog, char *value) {
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_INVALID_INPUT,
        package_catalog_build_url(NULL,
                                  value,
                                  MAX_CATALOG_URL_LEN,
                                  "compiler",
                                  "clang",
                                  "linux-x64",
                                  "linux-x64",
                                  "22.1.5",
                                  "tar.xz"));
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_INVALID_INPUT,
        package_catalog_build_url(catalog,
                                  NULL,
                                  MAX_CATALOG_URL_LEN,
                                  "compiler",
                                  "clang",
                                  "linux-x64",
                                  "linux-x64",
                                  "22.1.5",
                                  "tar.xz"));
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_INVALID_INPUT,
        package_catalog_build_url(catalog,
                                  value,
                                  0,
                                  "compiler",
                                  "clang",
                                  "linux-x64",
                                  "linux-x64",
                                  "22.1.5",
                                  "tar.xz"));
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_INVALID_INPUT,
        package_catalog_build_url(catalog,
                                  value,
                                  MAX_CATALOG_URL_LEN,
                                  "",
                                  "clang",
                                  "linux-x64",
                                  "linux-x64",
                                  "22.1.5",
                                  "tar.xz"));
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_INVALID_INPUT,
        package_catalog_build_url(catalog,
                                  value,
                                  MAX_CATALOG_URL_LEN,
                                  "compiler",
                                  "",
                                  "linux-x64",
                                  "linux-x64",
                                  "22.1.5",
                                  "tar.xz"));
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_INVALID_INPUT,
        package_catalog_build_url(catalog,
                                  value,
                                  MAX_CATALOG_URL_LEN,
                                  "compiler",
                                  "clang",
                                  "",
                                  "linux-x64",
                                  "22.1.5",
                                  "tar.xz"));
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_INVALID_INPUT,
        package_catalog_build_url(catalog,
                                  value,
                                  MAX_CATALOG_URL_LEN,
                                  "compiler",
                                  "clang",
                                  "linux-x64",
                                  "",
                                  "22.1.5",
                                  "tar.xz"));
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_INVALID_INPUT,
        package_catalog_build_url(catalog,
                                  value,
                                  MAX_CATALOG_URL_LEN,
                                  "compiler",
                                  "clang",
                                  "linux-x64",
                                  "linux-x64",
                                  "",
                                  "tar.xz"));
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_INVALID_INPUT,
        package_catalog_build_url(catalog,
                                  value,
                                  MAX_CATALOG_URL_LEN,
                                  "compiler",
                                  "clang",
                                  "linux-x64",
                                  "linux-x64",
                                  "22.1.5",
                                  ""));
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_INVALID_INPUT,
        package_catalog_build_checksum_url(catalog,
                                           value,
                                           MAX_CATALOG_URL_LEN,
                                           "compiler",
                                           "clang",
                                           "linux-x64",
                                           "linux-x64",
                                           ""));
}

static void assert_invalid_catalog_queries(PackageCatalog *catalog, char *value) {
    assert_invalid_catalog_load_and_resolution(catalog, value);
    assert_invalid_catalog_stability(catalog);
    assert_invalid_catalog_package_presence(catalog);
    assert_invalid_catalog_version_presence(catalog);
    assert_invalid_catalog_format_presence(catalog);
    assert_invalid_catalog_default_format(catalog, value);
    assert_invalid_catalog_urls(catalog, value);
}

static void assert_missing_catalog_queries(PackageCatalog *catalog, char *value) {
    int flag = 1;

    strcpy(value, "stale");
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_CATALOG,
        package_catalog_resolve_stable(
            catalog, value, MAX_CATALOG_URL_LEN, "compiler", "gcc", "linux-x64", "linux-x64"));
    TEST_ASSERT_EQUAL_STRING("", value);
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_CATALOG,
        package_catalog_is_stable(
            catalog, "compiler", "gcc", "linux-x64", "linux-x64", "1", &flag));
    TEST_ASSERT_FALSE(flag);
    flag = 1;
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_CATALOG,
        package_catalog_has_version(
            catalog, "compiler", "gcc", "linux-x64", "linux-x64", "1", &flag));
    TEST_ASSERT_FALSE(flag);
    strcpy(value, "stale");
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_CATALOG,
        package_catalog_get_default_format(
            catalog, value, MAX_CATALOG_URL_LEN, "compiler", "gcc", "linux-x64", "linux-x64"));
    TEST_ASSERT_EQUAL_STRING("", value);
    flag = 1;
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_CATALOG,
        package_catalog_has_format(
            catalog, "compiler", "gcc", "linux-x64", "linux-x64", "zip", &flag));
    TEST_ASSERT_FALSE(flag);
    strcpy(value, "stale");
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_CATALOG,
        package_catalog_build_url(catalog,
                                  value,
                                  MAX_CATALOG_URL_LEN,
                                  "compiler",
                                  "gcc",
                                  "linux-x64",
                                  "linux-x64",
                                  "1",
                                  "zip"));
    TEST_ASSERT_EQUAL_STRING("", value);
    strcpy(value, "stale");
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_CATALOG,
        package_catalog_build_checksum_url(catalog,
                                           value,
                                           MAX_CATALOG_URL_LEN,
                                           "compiler",
                                           "gcc",
                                           "linux-x64",
                                           "linux-x64",
                                           "1"));
    TEST_ASSERT_EQUAL_STRING("", value);
}

static void assert_catalog_query_bounds(PackageCatalog *catalog, char *value) {
    char huge[MAX_CATALOG_URL_LEN + 32];

    strcpy(value, "stale");
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_BUFFER_TOO_SMALL,
        package_catalog_resolve_stable(
            catalog, value, 2, "compiler", "clang", "linux-x64", "linux-x64"));
    TEST_ASSERT_EQUAL_STRING("", value);
    strcpy(value, "stale");
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_BUFFER_TOO_SMALL,
        package_catalog_get_default_format(
            catalog, value, 2, "compiler", "clang", "linux-x64", "linux-x64"));
    TEST_ASSERT_EQUAL_STRING("", value);
    strcpy(value, "stale");
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_BUFFER_TOO_SMALL,
        package_catalog_build_url(
            catalog, value, 4, "compiler", "clang", "linux-x64", "linux-x64", "22.1.5", "tar.xz"));
    TEST_ASSERT_EQUAL_STRING("", value);

    memset(huge, 'v', sizeof(huge) - 1);
    huge[sizeof(huge) - 1] = '\0';
    strcpy(value, "stale");
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_BUFFER_TOO_SMALL,
        package_catalog_build_url(catalog,
                                  value,
                                  MAX_CATALOG_URL_LEN,
                                  "compiler",
                                  "clang",
                                  "linux-x64",
                                  "linux-x64",
                                  huge,
                                  "tar.xz"));
    TEST_ASSERT_EQUAL_STRING("", value);
    strcpy(value, "stale");
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_BUFFER_TOO_SMALL,
        package_catalog_build_checksum_url(catalog,
                                           value,
                                           MAX_CATALOG_URL_LEN,
                                           "compiler",
                                           "clang",
                                           "linux-x64",
                                           "linux-x64",
                                           huge));
    TEST_ASSERT_EQUAL_STRING("", value);
}

static void test_query_errors(void) {
    PackageCatalog catalog;
    char path[256];
    char value[MAX_CATALOG_URL_LEN];

    package_catalog_init(&catalog);
    build_path(path, sizeof(path), "queries.cfg");
    write_valid_catalog(path);
    TEST_ASSERT_EQUAL_INT(
        CUP_OK, package_catalog_load_path(&catalog, path));

    assert_invalid_catalog_queries(&catalog, value);
    assert_missing_catalog_queries(&catalog, value);
    assert_catalog_query_bounds(&catalog, value);
    package_catalog_free(&catalog);
}

static void test_load_failures(void) {
    PackageCatalog catalog;
    char path[256];
    char long_path[MAX_PATH_LEN + 8];
    unsigned char bad[] = {'k', 'e', 'y', '=', 'v', 1, '\n'};
    FILE *file;

    package_catalog_init(NULL);
    package_catalog_free(NULL);
    package_catalog_init(&catalog);
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          package_catalog_load_path(NULL, "x"));
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_INVALID_INPUT,
        package_catalog_load_path(&catalog, ""));
    build_path(path, sizeof(path), "missing.cfg");
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_CATALOG,
        package_catalog_load_path(&catalog, path));

    memset(long_path, 'p', sizeof(long_path) - 1);
    long_path[sizeof(long_path) - 1] = '\0';
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_FILESYSTEM,
        package_catalog_load_path(&catalog, long_path));

    build_path(path, sizeof(path), "bad-byte.cfg");
    file = fopen(path, "wb");
    TEST_ASSERT_NOT_NULL(file);
    TEST_ASSERT_EQUAL_size_t(9, fwrite("format=1\n", 1, 9, file));
    TEST_ASSERT_EQUAL_size_t(sizeof(bad), fwrite(bad, 1, sizeof(bad), file));
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_CATALOG,
        package_catalog_load_path(&catalog, path));
    package_catalog_free(&catalog);
}


int main(void) {
    TEST_ASSERT_NOT_NULL(test_make_temp_directory(
        temp_dir, sizeof(temp_dir), "cup-catalog-test"));
    UNITY_BEGIN();
    RUN_TEST(test_load_queries);
    RUN_TEST(test_source_choice);
    RUN_TEST(test_tuple_growth);
    RUN_TEST(test_record_errors);
    RUN_TEST(test_value_errors);
    RUN_TEST(test_template_errors);
    RUN_TEST(test_query_errors);
    RUN_TEST(test_load_failures);
    return UNITY_END();
}
