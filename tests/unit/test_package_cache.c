/*
 * Test focus: Exercises transfer validation, limits, TLS/timeout mapping, cache refresh and
 * checksum policy with libcurl/system boundaries simulated.
 */

#include "checksum.h"
#include "package_cache.h"
#include "download.h"
#include "constants.h"
#include "error.h"
#include "filesystem.h"
#include "layout.h"
#include "package_archive.h"
#include "system.h"
#include "unity.h"

#include <curl/curl.h>

#undef curl_easy_setopt
#undef curl_easy_getinfo
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAX_SEQUENCE 16

/*
 * Scenario controls and observations. Configured results drive the boundary doubles below;
 * counters record the calls made by production code.
 */

static char temp_home[] = "/tmp/cup-package-cache-test-XXXXXX";
static CURLcode mock_global_result;
static CURLcode mock_perform_result;
static CURLcode mock_info_result;
static CURLoption mock_fail_option;
static long mock_response_code;
static int mock_easy_init_null;
static int mock_interrupt;
static int mock_too_large;
static int mock_overflow;
static char mock_payload[64];
static const char *mock_fail_url;
static char *mock_error_buffer;
static char mock_url[1024];
static curl_write_callback mock_write_callback;
static void *mock_write_userdata;
static curl_xferinfo_callback mock_progress_callback;
static void *mock_progress_userdata;

static CupError archive_results[MAX_SEQUENCE];
static int archive_values[MAX_SEQUENCE];
static size_t archive_count;
static size_t archive_index;
static size_t archive_format_calls;
static CupError find_results[MAX_SEQUENCE];
static size_t find_count;
static size_t find_index;
static CupError verify_results[MAX_SEQUENCE];
static int verify_values[MAX_SEQUENCE];
static size_t verify_count;
static size_t verify_index;

/* Fixture lifecycle and local construction helpers. */

static void reset_mocks(void) {
    mock_global_result = CURLE_OK;
    mock_perform_result = CURLE_OK;
    mock_info_result = CURLE_OK;
    mock_fail_option = (CURLoption)-1;
    mock_response_code = 200;
    mock_easy_init_null = 0;
    mock_interrupt = 0;
    mock_too_large = 0;
    mock_overflow = 0;
    strcpy(mock_payload, "downloaded data\n");
    mock_fail_url = NULL;
    mock_error_buffer = NULL;
    mock_url[0] = '\0';
    mock_write_callback = NULL;
    mock_write_userdata = NULL;
    mock_progress_callback = NULL;
    mock_progress_userdata = NULL;
    archive_count = 0;
    archive_index = 0;
    archive_format_calls = 0;
    find_count = 0;
    find_index = 0;
    verify_count = 0;
    verify_index = 0;
}

void setUp(void) {
    reset_mocks();
}

void tearDown(void) {
}

/*
 * Controlled boundary doubles. Each implementation exposes one dependency through the scenario
 * state above.
 */

CURLcode curl_global_init(long flags) {
    (void)flags;
    return mock_global_result;
}

void curl_global_cleanup(void) {
}

CURL *curl_easy_init(void) {
    return mock_easy_init_null ? NULL : (CURL *)(uintptr_t)1;
}

void curl_easy_cleanup(CURL *curl) {
    (void)curl;
}

CURLcode curl_easy_setopt(CURL *curl, CURLoption option, ...) {
    va_list args;
    (void)curl;

    if (option == mock_fail_option) {
        return CURLE_UNKNOWN_OPTION;
    }

    va_start(args, option);
    switch (option) {
        case CURLOPT_ERRORBUFFER:
            mock_error_buffer = va_arg(args, char *);
            break;
        case CURLOPT_URL: {
            const char *url = va_arg(args, const char *);
            if (url != NULL) {
                (void)snprintf(mock_url, sizeof(mock_url), "%s", url);
            }
            break;
        }
        case CURLOPT_WRITEFUNCTION:
            mock_write_callback = va_arg(args, curl_write_callback);
            break;
        case CURLOPT_WRITEDATA:
            mock_write_userdata = va_arg(args, void *);
            break;
        case CURLOPT_XFERINFOFUNCTION:
            mock_progress_callback = va_arg(args, curl_xferinfo_callback);
            break;
        case CURLOPT_XFERINFODATA:
            mock_progress_userdata = va_arg(args, void *);
            break;
        default:
            /* The mock ignores options that do not affect the observed transfer contract. */
            break;
    }
    va_end(args);
    return CURLE_OK;
}

CURLcode curl_easy_perform(CURL *curl) {
    static char checksum_payload[] = "mock checksum metadata\n";
    static char archive_payload[] = "mock package archive\n";
    char one_byte = 'x';
    char *payload = mock_payload;
    size_t length;
    size_t written;
    (void)curl;

    if (mock_fail_url == NULL || strstr(mock_url, mock_fail_url) != NULL) {
        if (mock_error_buffer != NULL && mock_perform_result != CURLE_OK) {
            (void)snprintf(mock_error_buffer, CURL_ERROR_SIZE, "mock curl error");
        }
        if (mock_perform_result != CURLE_OK) {
            return mock_perform_result;
        }
    }
    if (mock_progress_callback != NULL &&
        mock_progress_callback(mock_progress_userdata, 1, 0, 0, 0) != 0) {
        return CURLE_ABORTED_BY_CALLBACK;
    }
    if (mock_write_callback == NULL) {
        return CURLE_WRITE_ERROR;
    }
    if (mock_too_large) {
        (void)mock_write_callback(
            &one_byte, 1, (size_t)MAX_METADATA_DOWNLOAD_BYTES + 1, mock_write_userdata);
        return CURLE_WRITE_ERROR;
    }
    if (mock_overflow) {
        (void)mock_write_callback(&one_byte, SIZE_MAX, 2, mock_write_userdata);
        return CURLE_WRITE_ERROR;
    }
    if (strstr(mock_url, "checksum") != NULL) {
        payload = checksum_payload;
    } else if (strstr(mock_url, "archive") != NULL) {
        payload = archive_payload;
    }
    length = strlen(payload);
    written = mock_write_callback(payload, 1, length, mock_write_userdata);
    return written == length ? CURLE_OK : CURLE_WRITE_ERROR;
}

CURLcode curl_easy_getinfo(CURL *curl, CURLINFO info, ...) {
    va_list args;
    long *value;
    (void)curl;
    (void)info;

    va_start(args, info);
    value = va_arg(args, long *);
    if (value != NULL) {
        *value = mock_response_code;
    }
    va_end(args);
    return mock_info_result;
}

const char *curl_easy_strerror(CURLcode code) {
    (void)code;
    return "mock curl error";
}

int interrupt_requested(void) {
    return mock_interrupt;
}

CupError package_archive_is_valid(const char *archive_path,
                                  const char *expected_format,
                                  int *is_valid) {
    CupError result = CUP_OK;
    int value = access(archive_path, F_OK) == 0;

    if (expected_format != NULL) {
        TEST_ASSERT_EQUAL_STRING("tar.gz", expected_format);
        archive_format_calls++;
    }

    if (archive_index < archive_count) {
        result = archive_results[archive_index];
        value = archive_values[archive_index];
        archive_index++;
    }
    if (is_valid != NULL) {
        *is_valid = value;
    }
    return result;
}

CupError checksum_find_expected(const char *checksum_path,
                                const char *asset_name,
                                char *hex,
                                size_t size) {
    CupError result = CUP_OK;
    (void)checksum_path;
    (void)asset_name;

    if (find_index < find_count) {
        result = find_results[find_index++];
    }
    if (result == CUP_OK && hex != NULL && size >= SHA256_HEX_LENGTH + 1) {
        memset(hex, 'a', SHA256_HEX_LENGTH);
        hex[SHA256_HEX_LENGTH] = '\0';
    }
    return result;
}

CupError checksum_verify_file(const char *checksum_path,
                              const char *asset_name,
                              const char *asset_path,
                              int *matches) {
    CupError result = CUP_OK;
    int value = 1;
    (void)checksum_path;
    (void)asset_name;
    (void)asset_path;

    if (verify_index < verify_count) {
        result = verify_results[verify_index];
        value = verify_values[verify_index];
        verify_index++;
    }
    if (matches != NULL) {
        *matches = value;
    }
    return result;
}

static void push_archive(CupError result, int valid) {
    TEST_ASSERT_TRUE(archive_count < MAX_SEQUENCE);
    archive_results[archive_count] = result;
    archive_values[archive_count] = valid;
    archive_count++;
}

static void push_find(CupError result) {
    TEST_ASSERT_TRUE(find_count < MAX_SEQUENCE);
    find_results[find_count++] = result;
}

static void push_verify(CupError result, int matches) {
    TEST_ASSERT_TRUE(verify_count < MAX_SEQUENCE);
    verify_results[verify_count] = result;
    verify_values[verify_count] = matches;
    verify_count++;
}

static void build_path(char *buffer, size_t size, const char *name) {
    int written = snprintf(buffer, size, "%s/%s", temp_home, name);
    TEST_ASSERT_TRUE(written >= 0 && (size_t)written < size);
}

static void write_text(const char *path, const char *text) {
    FILE *file = fopen(path, "w");
    TEST_ASSERT_NOT_NULL(file);
    TEST_ASSERT_EQUAL_size_t(strlen(text), fwrite(text, 1, strlen(text), file));
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
}

static void read_text(const char *path, char *buffer, size_t size) {
    FILE *file = fopen(path, "r");
    size_t count;

    TEST_ASSERT_NOT_NULL(file);
    count = fread(buffer, 1, size - 1, file);
    TEST_ASSERT_FALSE(ferror(file));
    buffer[count] = '\0';
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
}

static PackageIdentity identity_for(const char *version) {
    PackageIdentity identity;

    memset(&identity, 0, sizeof(identity));
    TEST_ASSERT_TRUE(snprintf(identity.component, sizeof(identity.component), "compiler") > 0);
    TEST_ASSERT_TRUE(snprintf(identity.tool, sizeof(identity.tool), "clang") > 0);
    TEST_ASSERT_TRUE(snprintf(identity.host_platform, sizeof(identity.host_platform), "linux-x64") >
                     0);
    TEST_ASSERT_TRUE(
        snprintf(identity.target_platform, sizeof(identity.target_platform), "linux-x64") > 0);
    TEST_ASSERT_TRUE(snprintf(identity.version, sizeof(identity.version), "%s", version) > 0);
    return identity;
}

static void make_cache_files(const PackageIdentity *identity,
                             char *archive_path,
                             size_t archive_size) {
    char checksum_path[1024];
    char *slash;

    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_ensure_cache_parent(identity));
    TEST_ASSERT_EQUAL_INT(
        CUP_OK, layout_build_cache_archive_path(archive_path, archive_size, identity, "tar.gz"));
    TEST_ASSERT_TRUE(snprintf(checksum_path, sizeof(checksum_path), "%s", archive_path) > 0);
    slash = strrchr(checksum_path, '/');
    TEST_ASSERT_NOT_NULL(slash);
    TEST_ASSERT_TRUE(snprintf(slash + 1,
                              sizeof(checksum_path) - (size_t)(slash + 1 - checksum_path),
                              "SHA256SUMS") > 0);
    write_text(checksum_path, "mock checksum metadata\n");
    write_text(archive_path, "mock package archive\n");
}

/*
 * Test cases exercise the real production entry point while changing only controlled boundary
 * outcomes.
 */

static void test_file_success(void) {
    char destination[1024];
    char content[128];
    DownloadValidation validations[] = {DOWNLOAD_VALIDATE_NONEMPTY,
                                        DOWNLOAD_VALIDATE_METADATA,
                                        DOWNLOAD_VALIDATE_BINARY,
                                        DOWNLOAD_VALIDATE_ARCHIVE};
    const char *destinations[] = {"nonempty.out", "metadata.out", "binary.out", "archive.out"};
    size_t i;

    for (i = 0; i < sizeof(validations) / sizeof(validations[0]); ++i) {
        reset_mocks();
        build_path(destination, sizeof(destination), destinations[i]);
        if (validations[i] == DOWNLOAD_VALIDATE_ARCHIVE) {
            push_archive(CUP_OK, 1);
        }
        TEST_ASSERT_EQUAL_INT(
            CUP_OK, download_file("https://example.invalid/resource", destination, validations[i]));
        read_text(destination, content, sizeof(content));
        TEST_ASSERT_EQUAL_STRING("downloaded data\n", content);
    }

    reset_mocks();
    build_path(destination, sizeof(destination), "readonly.out");
    write_text(destination, "old\n");
    TEST_ASSERT_EQUAL_INT(0, chmod(destination, 0444));
    TEST_ASSERT_EQUAL_INT(
        CUP_OK,
        download_file("https://example.invalid/resource", destination, DOWNLOAD_VALIDATE_NONEMPTY));
    read_text(destination, content, sizeof(content));
    TEST_ASSERT_EQUAL_STRING("downloaded data\n", content);
}

static void assert_download_argument_failures(const char *destination) {
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          download_file(NULL, destination, DOWNLOAD_VALIDATE_NONEMPTY));
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_INVALID_INPUT,
        download_file("https://example.invalid", NULL, DOWNLOAD_VALIDATE_NONEMPTY));
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_INVALID_INPUT,
        download_file("https://example.invalid", destination, (DownloadValidation)999));
}

static void assert_download_setup_failures(const char *destination) {
    char missing_parent[1024];

    reset_mocks();
    mock_global_result = CURLE_FAILED_INIT;
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_FETCH,
        download_file("https://example.invalid", destination, DOWNLOAD_VALIDATE_NONEMPTY));

    reset_mocks();
    build_path(missing_parent, sizeof(missing_parent), "missing/child.out");
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_FETCH,
        download_file("https://example.invalid", missing_parent, DOWNLOAD_VALIDATE_NONEMPTY));

    reset_mocks();
    mock_easy_init_null = 1;
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_FETCH,
        download_file("https://example.invalid", destination, DOWNLOAD_VALIDATE_NONEMPTY));

    reset_mocks();
    mock_fail_option = CURLOPT_URL;
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_FETCH,
        download_file("https://example.invalid", destination, DOWNLOAD_VALIDATE_NONEMPTY));
}

static void assert_download_transport_failures(const char *destination) {
    reset_mocks();
    mock_perform_result = CURLE_OPERATION_TIMEDOUT;
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_TIMEOUT,
        download_file("https://example.invalid", destination, DOWNLOAD_VALIDATE_NONEMPTY));

    reset_mocks();
    mock_perform_result = CURLE_PEER_FAILED_VERIFICATION;
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_TLS,
        download_file("https://example.invalid", destination, DOWNLOAD_VALIDATE_NONEMPTY));

    reset_mocks();
    mock_response_code = 404;
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_FETCH,
        download_file("https://example.invalid", destination, DOWNLOAD_VALIDATE_NONEMPTY));

    reset_mocks();
    mock_interrupt = 1;
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_INTERRUPT,
        download_file("https://example.invalid", destination, DOWNLOAD_VALIDATE_NONEMPTY));

    reset_mocks();
    mock_info_result = CURLE_BAD_FUNCTION_ARGUMENT;
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_FETCH,
        download_file("https://example.invalid", destination, DOWNLOAD_VALIDATE_NONEMPTY));

    reset_mocks();
    mock_perform_result = CURLE_SSL_CONNECT_ERROR;
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_TLS,
        download_file("https://example.invalid", destination, DOWNLOAD_VALIDATE_NONEMPTY));
}

static void assert_download_content_failures(const char *destination) {
    reset_mocks();
    mock_too_large = 1;
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_DOWNLOAD_TOO_LARGE,
        download_file("https://example.invalid", destination, DOWNLOAD_VALIDATE_METADATA));

    reset_mocks();
    mock_overflow = 1;
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_FILESYSTEM,
        download_file("https://example.invalid", destination, DOWNLOAD_VALIDATE_NONEMPTY));

    reset_mocks();
    mock_payload[0] = '\0';
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_FETCH,
        download_file("https://example.invalid", destination, DOWNLOAD_VALIDATE_NONEMPTY));

    reset_mocks();
    push_archive(CUP_OK, 0);
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_ARCHIVE,
        download_file("https://example.invalid", destination, DOWNLOAD_VALIDATE_ARCHIVE));

    reset_mocks();
    push_archive(CUP_ERR_FILESYSTEM, 0);
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_FILESYSTEM,
        download_file("https://example.invalid", destination, DOWNLOAD_VALIDATE_ARCHIVE));

    reset_mocks();
    mock_perform_result = CURLE_FILESIZE_EXCEEDED;
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_DOWNLOAD_TOO_LARGE,
        download_file("https://example.invalid", destination, DOWNLOAD_VALIDATE_BINARY));
}

static void assert_download_destination_failures(const char *destination) {
    char long_path[MAX_PATH_LEN + 32];

    reset_mocks();
    memset(long_path, 'a', sizeof(long_path) - 1);
    long_path[sizeof(long_path) - 1] = '\0';
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_BUFFER_TOO_SMALL,
        download_file("https://example.invalid", long_path, DOWNLOAD_VALIDATE_NONEMPTY));

    reset_mocks();
    TEST_ASSERT_EQUAL_INT(0, mkdir(destination, 0755));
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_FILESYSTEM,
        download_file("https://example.invalid", destination, DOWNLOAD_VALIDATE_NONEMPTY));
}

static void test_file_failures(void) {
    char destination[1024];

    build_path(destination, sizeof(destination), "errors.out");
    assert_download_argument_failures(destination);
    assert_download_setup_failures(destination);
    assert_download_transport_failures(destination);
    assert_download_content_failures(destination);

    build_path(destination, sizeof(destination), "destination-directory");
    assert_download_destination_failures(destination);
}

static void test_fetch_cache_refresh(void) {
    PackageIdentity identity = identity_for("22.1.5-test-cache");
    PackageCacheSource source;
    char archive_path[1024];

    make_cache_files(&identity, archive_path, sizeof(archive_path));
    push_find(CUP_OK);
    push_archive(CUP_OK, 1);
    push_verify(CUP_OK, 1);
    TEST_ASSERT_EQUAL_INT(CUP_OK,
                          package_cache_fetch(archive_path,
                                              sizeof(archive_path),
                                              "https://example.invalid/archive",
                                              "https://example.invalid/checksum",
                                              &identity,
                                              "tar.gz",
                                              PACKAGE_CACHE_ALLOW,
                                              &source));
    TEST_ASSERT_EQUAL_INT(PACKAGE_CACHE_SOURCE_CACHE, source);
    TEST_ASSERT_TRUE(archive_format_calls > 0);

    reset_mocks();
    identity = identity_for("22.1.5-test-network");
    push_find(CUP_OK);
    push_archive(CUP_OK, 0);
    push_archive(CUP_OK, 1);
    push_verify(CUP_OK, 1);
    TEST_ASSERT_EQUAL_INT(CUP_OK,
                          package_cache_fetch(archive_path,
                                              sizeof(archive_path),
                                              "https://example.invalid/archive",
                                              "https://example.invalid/checksum",
                                              &identity,
                                              "tar.gz",
                                              PACKAGE_CACHE_ALLOW,
                                              &source));
    TEST_ASSERT_EQUAL_INT(PACKAGE_CACHE_SOURCE_NETWORK, source);

    reset_mocks();
    identity = identity_for("22.1.5-test-refresh-entry");
    make_cache_files(&identity, archive_path, sizeof(archive_path));
    push_find(CUP_ERR_VALIDATION);
    push_find(CUP_OK);
    push_archive(CUP_OK, 1);
    push_verify(CUP_OK, 1);
    TEST_ASSERT_EQUAL_INT(CUP_OK,
                          package_cache_fetch(archive_path,
                                              sizeof(archive_path),
                                              "https://example.invalid/archive",
                                              "https://example.invalid/checksum",
                                              &identity,
                                              "tar.gz",
                                              PACKAGE_CACHE_ALLOW,
                                              &source));
    TEST_ASSERT_EQUAL_INT(PACKAGE_CACHE_SOURCE_CACHE, source);

    reset_mocks();
    identity = identity_for("22.1.5-test-forced-refresh");
    push_find(CUP_OK);
    push_archive(CUP_OK, 1);
    push_verify(CUP_OK, 1);
    TEST_ASSERT_EQUAL_INT(CUP_OK,
                          package_cache_fetch(archive_path,
                                              sizeof(archive_path),
                                              "https://example.invalid/archive",
                                              "https://example.invalid/checksum",
                                              &identity,
                                              "tar.gz",
                                              PACKAGE_CACHE_REFRESH,
                                              &source));
    TEST_ASSERT_EQUAL_INT(PACKAGE_CACHE_SOURCE_NETWORK, source);
}

static void test_stale_cache(void) {
    PackageIdentity identity;
    PackageCacheSource source;
    char archive_path[1024];

    identity = identity_for("22.1.5-test-stale-valid");
    make_cache_files(&identity, archive_path, sizeof(archive_path));
    push_find(CUP_OK);
    push_archive(CUP_OK, 1);
    push_verify(CUP_OK, 0);
    push_find(CUP_OK);
    push_archive(CUP_OK, 1);
    push_verify(CUP_OK, 1);
    TEST_ASSERT_EQUAL_INT(CUP_OK,
                          package_cache_fetch(archive_path,
                                              sizeof(archive_path),
                                              "https://example.invalid/archive",
                                              "https://example.invalid/checksum",
                                              &identity,
                                              "tar.gz",
                                              PACKAGE_CACHE_ALLOW,
                                              &source));
    TEST_ASSERT_EQUAL_INT(PACKAGE_CACHE_SOURCE_CACHE, source);

    reset_mocks();
    identity = identity_for("22.1.5-test-stale-error");
    make_cache_files(&identity, archive_path, sizeof(archive_path));
    push_find(CUP_OK);
    push_archive(CUP_OK, 1);
    push_verify(CUP_ERR_INTERRUPT, 0);
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INTERRUPT,
                          package_cache_fetch(archive_path,
                                              sizeof(archive_path),
                                              "https://example.invalid/archive",
                                              "https://example.invalid/checksum",
                                              &identity,
                                              "tar.gz",
                                              PACKAGE_CACHE_ALLOW,
                                              &source));

    reset_mocks();
    identity = identity_for("22.1.5-test-refresh-fail");
    make_cache_files(&identity, archive_path, sizeof(archive_path));
    push_find(CUP_ERR_VALIDATION);
    mock_fail_url = "checksum";
    mock_perform_result = CURLE_COULDNT_CONNECT;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FETCH,
                          package_cache_fetch(archive_path,
                                              sizeof(archive_path),
                                              "https://example.invalid/archive",
                                              "https://example.invalid/checksum",
                                              &identity,
                                              "tar.gz",
                                              PACKAGE_CACHE_ALLOW,
                                              &source));

    reset_mocks();
    identity = identity_for("22.1.5-test-network-fail");
    mock_fail_url = "archive";
    mock_perform_result = CURLE_COULDNT_CONNECT;
    push_find(CUP_OK);
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FETCH,
                          package_cache_fetch(archive_path,
                                              sizeof(archive_path),
                                              "https://example.invalid/archive",
                                              "https://example.invalid/checksum",
                                              &identity,
                                              "tar.gz",
                                              PACKAGE_CACHE_REFRESH,
                                              &source));
}

static void test_network_recheck(void) {
    PackageIdentity identity = identity_for("22.1.5-test-recheck");
    PackageCacheSource source;
    char archive_path[1024];
    char checksum_path[1024];
    char *slash;

    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_ensure_cache_parent(&identity));
    TEST_ASSERT_EQUAL_INT(
        CUP_OK,
        layout_build_cache_archive_path(archive_path, sizeof(archive_path), &identity, "tar.gz"));
    TEST_ASSERT_TRUE(snprintf(checksum_path, sizeof(checksum_path), "%s", archive_path) > 0);
    slash = strrchr(checksum_path, '/');
    TEST_ASSERT_NOT_NULL(slash);
    TEST_ASSERT_TRUE(snprintf(slash + 1,
                              sizeof(checksum_path) - (size_t)(slash + 1 - checksum_path),
                              "SHA256SUMS") > 0);
    write_text(checksum_path, "old checksums\n");

    push_find(CUP_OK);
    push_archive(CUP_ERR_ARCHIVE, 0);
    push_archive(CUP_OK, 1);
    push_archive(CUP_OK, 1);
    push_verify(CUP_OK, 0);
    push_find(CUP_OK);
    push_archive(CUP_OK, 1);
    push_verify(CUP_OK, 1);
    TEST_ASSERT_EQUAL_INT(CUP_OK,
                          package_cache_fetch(archive_path,
                                              sizeof(archive_path),
                                              "https://example.invalid/archive",
                                              "https://example.invalid/checksum",
                                              &identity,
                                              "tar.gz",
                                              PACKAGE_CACHE_ALLOW,
                                              &source));
    TEST_ASSERT_EQUAL_INT(PACKAGE_CACHE_SOURCE_NETWORK, source);
}

static void test_package_failures(void) {
    PackageIdentity identity = identity_for("22.1.5-test-invalid");
    PackageCacheSource source;
    char archive_path[1024];
    char checksum_path[1024];
    char *slash;

    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_INVALID_INPUT,
        package_cache_fetch(NULL, 0, "u", "c", &identity, "tar.gz", PACKAGE_CACHE_ALLOW, &source));
    source = PACKAGE_CACHE_SOURCE_NETWORK;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          package_cache_fetch(archive_path,
                                              sizeof(archive_path),
                                              "u",
                                              "c",
                                              &identity,
                                              "tar.gz",
                                              (PackageCachePolicy)999,
                                              &source));
    TEST_ASSERT_EQUAL_INT(PACKAGE_CACHE_SOURCE_NONE, source);

    reset_mocks();
    identity = identity_for("22.1.5-test-no-entry");
    push_find(CUP_ERR_VALIDATION);
    push_find(CUP_ERR_VALIDATION);
    TEST_ASSERT_EQUAL_INT(CUP_ERR_VALIDATION,
                          package_cache_fetch(archive_path,
                                              sizeof(archive_path),
                                              "https://example.invalid/archive",
                                              "https://example.invalid/checksum",
                                              &identity,
                                              "tar.gz",
                                              PACKAGE_CACHE_ALLOW,
                                              &source));

    reset_mocks();
    identity = identity_for("22.1.5-test-mismatch");
    push_find(CUP_OK);
    push_archive(CUP_OK, 1);
    push_archive(CUP_OK, 1);
    push_verify(CUP_OK, 0);
    TEST_ASSERT_EQUAL_INT(CUP_ERR_VALIDATION,
                          package_cache_fetch(archive_path,
                                              sizeof(archive_path),
                                              "https://example.invalid/archive",
                                              "https://example.invalid/checksum",
                                              &identity,
                                              "tar.gz",
                                              PACKAGE_CACHE_REFRESH,
                                              &source));
    TEST_ASSERT_TRUE(access(archive_path, F_OK) != 0);

    reset_mocks();
    identity = identity_for("22.1.5-test-checksum-dir");
    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_ensure_cache_parent(&identity));
    TEST_ASSERT_EQUAL_INT(
        CUP_OK,
        layout_build_cache_archive_path(archive_path, sizeof(archive_path), &identity, "tar.gz"));
    TEST_ASSERT_TRUE(snprintf(checksum_path, sizeof(checksum_path), "%s", archive_path) > 0);
    slash = strrchr(checksum_path, '/');
    TEST_ASSERT_NOT_NULL(slash);
    TEST_ASSERT_TRUE(snprintf(slash + 1,
                              sizeof(checksum_path) - (size_t)(slash + 1 - checksum_path),
                              "SHA256SUMS") > 0);
    TEST_ASSERT_EQUAL_INT(0, mkdir(checksum_path, 0755));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM,
                          package_cache_fetch(archive_path,
                                              sizeof(archive_path),
                                              "https://example.invalid/archive",
                                              "https://example.invalid/checksum",
                                              &identity,
                                              "tar.gz",
                                              PACKAGE_CACHE_ALLOW,
                                              &source));
}

static void test_cache_discard(void) {
    char path[1024];

    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, package_cache_discard(NULL));
    build_path(path, sizeof(path), "missing-cache");
    TEST_ASSERT_EQUAL_INT(CUP_OK, package_cache_discard(path));

    build_path(path, sizeof(path), "cache-directory");
    TEST_ASSERT_EQUAL_INT(0, mkdir(path, 0755));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM, package_cache_discard(path));

    build_path(path, sizeof(path), "readonly-cache");
    write_text(path, "archive\n");
    TEST_ASSERT_EQUAL_INT(0, chmod(path, 0444));
    TEST_ASSERT_EQUAL_INT(CUP_OK, package_cache_discard(path));
    TEST_ASSERT_TRUE(access(path, F_OK) != 0);
}

/* Suite registration. */

int main(void) {
    TEST_ASSERT_NOT_NULL(mkdtemp(temp_home));
    TEST_ASSERT_EQUAL_INT(0, setenv("HOME", temp_home, 1));
    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_ensure_runtime());

    UNITY_BEGIN();
    RUN_TEST(test_file_success);
    RUN_TEST(test_file_failures);
    RUN_TEST(test_fetch_cache_refresh);
    RUN_TEST(test_stale_cache);
    RUN_TEST(test_network_recheck);
    RUN_TEST(test_package_failures);
    RUN_TEST(test_cache_discard);
    return UNITY_END();
}
