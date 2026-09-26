/*
 * Exercises transfer validation, limits, TLS/timeout mapping and digest-authenticated
 * archive reuse with libcurl/system boundaries simulated.
 */

#include "checksum.h"
#include "package_cache.h"
#include "download.h"
#include "constants.h"
#include "error.h"
#include "filesystem.h"
#include "layout.h"
#include "package_catalog.h"
#include "platform.h"
#include "install_policy.h"
#include "state.h"
#include "system.h"
#include "unity.h"
#include "test_platform.h"

#include <curl/curl.h>

#undef curl_easy_setopt
#undef curl_easy_getinfo
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(CUP_USE_OPENSSL_INIT)
#include <openssl/ssl.h>

static int mock_tls_init_result = 1;

int OPENSSL_init_ssl(uint64_t options, const OPENSSL_INIT_SETTINGS *settings) {
    (void)options;
    (void)settings;
    return mock_tls_init_result;
}
#endif

#define MAX_SEQUENCE 16

static char temp_home[CUP_TEST_TEMP_PATH_SIZE];
static CURLcode mock_global_result;
static CURLcode mock_perform_result;
static CURLcode mock_info_result;
static CURLoption mock_fail_option;
static long mock_response_code;
static int mock_easy_init_null;
static int mock_interrupt;
static int mock_too_large;
static int mock_loopback_allowed;
static long mock_follow_location;
static long mock_max_redirects;
static char mock_payload[64];
static const char *mock_fail_url;
static char *mock_error_buffer;
static char mock_url[1024];
#if LIBCURL_VERSION_NUM >= 0x075500
static char mock_protocols[32];
static char mock_redirect_protocols[32];
#else
static long mock_protocols;
static long mock_redirect_protocols;
#endif
static void *mock_write_userdata;
static curl_off_t mock_max_filesize;
static curl_xferinfo_callback mock_progress_callback;
static void *mock_progress_userdata;
static curl_off_t mock_progress_total;
static curl_off_t mock_progress_downloaded;

static CupError artifact_open_results[MAX_SEQUENCE];
static ArtifactVerificationStatus artifact_open_statuses[MAX_SEQUENCE];
static size_t artifact_open_count;
static size_t artifact_open_index;
static CupError artifact_discard_result;
static size_t artifact_discard_calls;

/* layout.c links its strong markerless-root verifier into this suite. Cache tests never exercise
 * that verifier, so these boundary doubles keep the suite focused on cache behavior. */
int checksum_digest_is_canonical(const char *value) {
    size_t i;
    if (value == NULL || strlen(value) != 64u) return 0;
    for (i = 0; i < 64u; ++i) {
        if (!((value[i] >= '0' && value[i] <= '9') || (value[i] >= 'a' && value[i] <= 'f'))) return 0;
    }
    return 1;
}

CupError checksum_sha256_file(const char *path, char *hex, size_t size) {
    (void)path;
    if (hex != NULL && size > 0) {
        hex[0] = '\0';
    }
    return CUP_ERR_VALIDATION;
}

CupError package_identity_validate(const PackageIdentity *identity, FILE *diagnostics) {
    (void)diagnostics;
    return identity == NULL ? CUP_ERR_INVALID_INPUT : CUP_OK;
}

void verified_artifact_release(VerifiedArtifact *artifact) {
    if (artifact != NULL) {
        memset(artifact, 0, sizeof(*artifact));
    }
}

CupError verified_artifact_open(VerifiedArtifact *artifact,
                                const char *path,
                                const PackageArtifactSpec *spec,
                                ArtifactVerificationStatus *status) {
    CupError result = CUP_ERR_FILESYSTEM;
    ArtifactVerificationStatus verification = ARTIFACT_VERIFY_NONE;
    (void)spec;

    if (artifact_open_index < artifact_open_count) {
        result = artifact_open_results[artifact_open_index];
        verification = artifact_open_statuses[artifact_open_index];
        artifact_open_index++;
    }
    if (artifact != NULL && result == CUP_OK &&
        (verification == ARTIFACT_VERIFY_VALID ||
         verification == ARTIFACT_VERIFY_REJECTED ||
         verification == ARTIFACT_VERIFY_DIGEST_MISMATCH)) {
        artifact->file = (FILE *)(uintptr_t)1;
        (void)snprintf(artifact->path, sizeof(artifact->path), "%s", path);
    }
    if (status != NULL) {
        *status = verification;
    }
    return result;
}

CupError verified_artifact_discard(VerifiedArtifact *artifact) {
    CupError result = artifact_discard_result;

    artifact_discard_calls++;
    if (artifact != NULL) {
        if (result == CUP_OK && artifact->path[0] != '\0' &&
            test_remove_tree(artifact->path) != 0) {
            result = CUP_ERR_FILESYSTEM;
        }
        memset(artifact, 0, sizeof(*artifact));
    }
    return result;
}

void package_catalog_init(PackageCatalog *catalog) {
    if (catalog != NULL) {
        memset(catalog, 0, sizeof(*catalog));
    }
}

void package_catalog_free(PackageCatalog *catalog) {
    (void)catalog;
}

CupError package_catalog_load_path(PackageCatalog *catalog, const char *path) {
    (void)catalog;
    (void)path;
    return CUP_ERR_CATALOG;
}

void install_policy_init(InstallPolicy *policy) {
    if (policy != NULL) {
        memset(policy, 0, sizeof(*policy));
    }
}

CupError state_load_path(CupState *state,
                         StateFileStatus *status,
                         const char *path,
                         FILE *diagnostics) {
    (void)diagnostics;
    (void)path;
    if (state != NULL) {
        memset(state, 0, sizeof(*state));
    }
    if (status != NULL) {
        *status = STATE_FILE_MISSING;
    }
    return CUP_OK;
}

CupError state_validate(const CupState *state, FILE *diagnostics) {
    (void)diagnostics;
    (void)state;
    return CUP_OK;
}

const char *package_archive_format_name(PackageArchiveFormat format) {
    switch (format) {
        case PACKAGE_ARCHIVE_FORMAT_TAR_XZ:
            return "tar.xz";
        case PACKAGE_ARCHIVE_FORMAT_TAR_GZ:
            return "tar.gz";
        case PACKAGE_ARCHIVE_FORMAT_ZIP:
            return "zip";
        case PACKAGE_ARCHIVE_FORMAT_ANY:
        default:
            return NULL;
    }
}

static void set_test_environment(const char *name, const char *value);

static void reset_mocks(void) {
#if defined(CUP_USE_OPENSSL_INIT)
    mock_tls_init_result = 1;
#endif
    mock_global_result = CURLE_OK;
    mock_perform_result = CURLE_OK;
    mock_info_result = CURLE_OK;
    mock_fail_option = (CURLoption)-1;
    mock_response_code = 200;
    mock_easy_init_null = 0;
    mock_interrupt = 0;
    mock_too_large = 0;
    mock_loopback_allowed = 0;
    mock_follow_location = 0;
    mock_max_redirects = 0;
    strcpy(mock_payload, "downloaded data\n");
    mock_fail_url = NULL;
    mock_error_buffer = NULL;
    mock_url[0] = '\0';
#if LIBCURL_VERSION_NUM >= 0x075500
    mock_protocols[0] = '\0';
    mock_redirect_protocols[0] = '\0';
#else
    mock_protocols = 0;
    mock_redirect_protocols = 0;
#endif
    mock_write_userdata = NULL;
    mock_max_filesize = 0;
    mock_progress_callback = NULL;
    mock_progress_userdata = NULL;
    mock_progress_total = 1;
    mock_progress_downloaded = 0;
    artifact_open_count = 0;
    artifact_open_index = 0;
    artifact_discard_result = CUP_OK;
    artifact_discard_calls = 0;
}

void setUp(void) {
    reset_mocks();
}

void tearDown(void) {
    set_test_environment("CUP_INSTALL_BASE_URL", NULL);
    set_test_environment("CUP_INSTALL_ALLOW_INSECURE", NULL);
}

int download_insecure_loopback_is_allowed(const char *url) {
    (void)url;
    return mock_loopback_allowed;
}

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
#if LIBCURL_VERSION_NUM >= 0x075500
        case CURLOPT_PROTOCOLS_STR: {
            const char *protocols = va_arg(args, const char *);
            if (protocols != NULL) {
                (void)snprintf(mock_protocols, sizeof(mock_protocols), "%s", protocols);
            }
            break;
        }
        case CURLOPT_REDIR_PROTOCOLS_STR: {
            const char *protocols = va_arg(args, const char *);
            if (protocols != NULL) {
                (void)snprintf(
                    mock_redirect_protocols, sizeof(mock_redirect_protocols), "%s", protocols);
            }
            break;
        }
#else
        case CURLOPT_PROTOCOLS:
            mock_protocols = va_arg(args, long);
            break;
        case CURLOPT_REDIR_PROTOCOLS:
            mock_redirect_protocols = va_arg(args, long);
            break;
#endif
        case CURLOPT_FOLLOWLOCATION:
            mock_follow_location = va_arg(args, long);
            break;
        case CURLOPT_MAXREDIRS:
            mock_max_redirects = va_arg(args, long);
            break;
        case CURLOPT_WRITEDATA:
            mock_write_userdata = va_arg(args, void *);
            break;
        case CURLOPT_MAXFILESIZE_LARGE:
            mock_max_filesize = va_arg(args, curl_off_t);
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
    static char archive_payload[] = "mock package archive\n";
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
        mock_progress_callback(mock_progress_userdata,
                               mock_progress_total,
                               mock_progress_downloaded,
                               0,
                               0) != 0) {
        return CURLE_ABORTED_BY_CALLBACK;
    }
    if (mock_write_userdata == NULL) {
        return CURLE_WRITE_ERROR;
    }
    if (mock_too_large) {
        return CURLE_FILESIZE_EXCEEDED;
    }
    if (strstr(mock_url, "archive") != NULL) {
        payload = archive_payload;
    }
    length = strlen(payload);
    written = fwrite(payload, 1, length, (FILE *)mock_write_userdata);
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

CupError interrupt_safe_point(void) {
    return mock_interrupt ? CUP_ERR_INTERRUPT : CUP_OK;
}

static void push_artifact(CupError result, ArtifactVerificationStatus status) {
    TEST_ASSERT_TRUE(artifact_open_count < MAX_SEQUENCE);
    artifact_open_results[artifact_open_count] = result;
    artifact_open_statuses[artifact_open_count] = status;
    artifact_open_count++;
}

static void build_path(char *buffer, size_t size, const char *name) {
    int written = snprintf(buffer, size, "%s/%s", temp_home, name);
    TEST_ASSERT_TRUE(written >= 0 && (size_t)written < size);
}

static void write_text(const char *path, const char *text) {
    FILE *file = fopen(path, "wb");
    TEST_ASSERT_NOT_NULL(file);
    TEST_ASSERT_EQUAL_size_t(strlen(text), fwrite(text, 1, strlen(text), file));
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
}

static void set_test_environment(const char *name, const char *value) {
#if defined(_WIN32)
    TEST_ASSERT_EQUAL_INT(0, _putenv_s(name, value == NULL ? "" : value));
#else
    if (value == NULL) {
        TEST_ASSERT_EQUAL_INT(0, unsetenv(name));
    } else {
        TEST_ASSERT_EQUAL_INT(0, setenv(name, value, 1));
    }
#endif
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
    TEST_ASSERT_EQUAL_INT(
        CUP_OK, platform_get_host(identity.host_platform, sizeof(identity.host_platform)));
    TEST_ASSERT_TRUE(snprintf(identity.target_platform,
                              sizeof(identity.target_platform),
                              "%s",
                              identity.host_platform) > 0);
    TEST_ASSERT_TRUE(snprintf(identity.version, sizeof(identity.version), "%s", version) > 0);
    return identity;
}

static void make_cache_file(const PackageArtifactSpec *spec,
                            char *archive_path,
                            size_t archive_size) {
    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_ensure_cache());
    TEST_ASSERT_EQUAL_INT(
        CUP_OK, layout_build_cache_path(archive_path, archive_size, spec->artifact_sha256));
    write_text(archive_path, "mock package archive\n");
}

static void test_protocol_policy(void) {
    char destination[1024];

    build_path(destination, sizeof(destination), "protocol-policy.out");
    TEST_ASSERT_EQUAL_INT(
        CUP_OK,
        download_file("https://example.invalid/resource",
                      destination,
                      DOWNLOAD_VALIDATE_METADATA));
#if LIBCURL_VERSION_NUM >= 0x075500
    TEST_ASSERT_EQUAL_STRING("https", mock_protocols);
    TEST_ASSERT_EQUAL_STRING("https", mock_redirect_protocols);
#else
    TEST_ASSERT_EQUAL_INT(CURLPROTO_HTTPS, mock_protocols);
    TEST_ASSERT_EQUAL_INT(CURLPROTO_HTTPS, mock_redirect_protocols);
#endif
    TEST_ASSERT_EQUAL_INT(1, mock_follow_location);
    TEST_ASSERT_EQUAL_INT(10, mock_max_redirects);

    reset_mocks();
    mock_loopback_allowed = 1;
    TEST_ASSERT_EQUAL_INT(
        CUP_OK,
        download_file("http://127.0.0.1:18080/resource",
                      destination,
                      DOWNLOAD_VALIDATE_METADATA));
#if LIBCURL_VERSION_NUM >= 0x075500
    TEST_ASSERT_EQUAL_STRING("http", mock_protocols);
    TEST_ASSERT_EQUAL_STRING("http", mock_redirect_protocols);
#else
    TEST_ASSERT_EQUAL_INT(CURLPROTO_HTTP, mock_protocols);
    TEST_ASSERT_EQUAL_INT(CURLPROTO_HTTP, mock_redirect_protocols);
#endif
    TEST_ASSERT_EQUAL_INT(0, mock_follow_location);
    TEST_ASSERT_EQUAL_INT(0, mock_max_redirects);

    reset_mocks();
    TEST_ASSERT_EQUAL_INT(
        CUP_OK,
        download_file("http://example.invalid/resource",
                      destination,
                      DOWNLOAD_VALIDATE_METADATA));
#if LIBCURL_VERSION_NUM >= 0x075500
    TEST_ASSERT_EQUAL_STRING("https", mock_protocols);
    TEST_ASSERT_EQUAL_STRING("https", mock_redirect_protocols);
#else
    TEST_ASSERT_EQUAL_INT(CURLPROTO_HTTPS, mock_protocols);
    TEST_ASSERT_EQUAL_INT(CURLPROTO_HTTPS, mock_redirect_protocols);
#endif
    TEST_ASSERT_EQUAL_INT(1, mock_follow_location);
    TEST_ASSERT_EQUAL_INT(10, mock_max_redirects);
}


static CupError request_interrupt_after_validation(const char *temporary_path, void *userdata) {
    (void)temporary_path;
    (void)userdata;
    mock_interrupt = 1;
    return CUP_OK;
}

static void test_file_success(void) {
    char destination[1024];
    char content[128];
    DownloadValidation validations[] = {DOWNLOAD_VALIDATE_METADATA,
                                        DOWNLOAD_VALIDATE_METADATA,
                                        DOWNLOAD_VALIDATE_BINARY,
                                        DOWNLOAD_VALIDATE_ARCHIVE};
    const char *destinations[] = {"nonempty.out", "metadata.out", "binary.out", "archive.out"};
    size_t i;

    for (i = 0; i < sizeof(validations) / sizeof(validations[0]); ++i) {
        reset_mocks();
        build_path(destination, sizeof(destination), destinations[i]);
        TEST_ASSERT_EQUAL_INT(
            CUP_OK, download_file("https://example.invalid/resource", destination, validations[i]));
        read_text(destination, content, sizeof(content));
        TEST_ASSERT_EQUAL_STRING("downloaded data\n", content);
    }

    reset_mocks();
    build_path(destination, sizeof(destination), "readonly.out");
    write_text(destination, "old\n");
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_set_read_only(destination, 1));
    TEST_ASSERT_EQUAL_INT(
        CUP_OK,
        download_file("https://example.invalid/resource", destination, DOWNLOAD_VALIDATE_METADATA));
    read_text(destination, content, sizeof(content));
    TEST_ASSERT_EQUAL_STRING("downloaded data\n", content);
}

static void test_download_progress_output(void) {
    char destination[1024];
    char output[512];
    FILE *capture;
    int saved_stdout;
    size_t bytes;
    CupError result;

    reset_mocks();
    mock_progress_total = (curl_off_t)100 * 1024 * 1024;
    mock_progress_downloaded = (curl_off_t)10 * 1024 * 1024;
    build_path(destination, sizeof(destination), "progress-archive-early.out");

    capture = tmpfile();
    TEST_ASSERT_NOT_NULL(capture);
    fflush(stdout);
    saved_stdout = test_dup_fd(TEST_PLATFORM_STDOUT_FD);
    TEST_ASSERT_TRUE(saved_stdout >= 0);
    TEST_ASSERT_TRUE(
        test_dup2_fd(test_file_descriptor(capture), TEST_PLATFORM_STDOUT_FD) >= 0);
    result = download_file("https://example.invalid/archive",
                           destination,
                           DOWNLOAD_VALIDATE_ARCHIVE);
    fflush(stdout);
    TEST_ASSERT_TRUE(test_dup2_fd(saved_stdout, TEST_PLATFORM_STDOUT_FD) >= 0);
    TEST_ASSERT_EQUAL_INT(0, test_close_fd(saved_stdout));
    TEST_ASSERT_EQUAL_INT(CUP_OK, result);
    TEST_ASSERT_EQUAL_INT(0, fseek(capture, 0, SEEK_SET));
    bytes = fread(output, 1, sizeof(output) - 1, capture);
    output[bytes] = '\0';
    TEST_ASSERT_EQUAL_INT(0, fclose(capture));
    TEST_ASSERT_EQUAL_STRING("", output);

    reset_mocks();
    mock_progress_total = (curl_off_t)100 * 1024 * 1024;
    mock_progress_downloaded = (curl_off_t)25 * 1024 * 1024;
    build_path(destination, sizeof(destination), "progress-archive.out");

    capture = tmpfile();
    TEST_ASSERT_NOT_NULL(capture);
    fflush(stdout);
    saved_stdout = test_dup_fd(TEST_PLATFORM_STDOUT_FD);
    TEST_ASSERT_TRUE(saved_stdout >= 0);
    TEST_ASSERT_TRUE(
        test_dup2_fd(test_file_descriptor(capture), TEST_PLATFORM_STDOUT_FD) >= 0);
    result = download_file("https://example.invalid/archive",
                           destination,
                           DOWNLOAD_VALIDATE_ARCHIVE);
    fflush(stdout);
    TEST_ASSERT_TRUE(test_dup2_fd(saved_stdout, TEST_PLATFORM_STDOUT_FD) >= 0);
    TEST_ASSERT_EQUAL_INT(0, test_close_fd(saved_stdout));
    TEST_ASSERT_EQUAL_INT(CUP_OK, result);
    TEST_ASSERT_EQUAL_INT(0, fseek(capture, 0, SEEK_SET));
    bytes = fread(output, 1, sizeof(output) - 1, capture);
    output[bytes] = '\0';
    TEST_ASSERT_EQUAL_INT(0, fclose(capture));
    TEST_ASSERT_NOT_NULL(strstr(
        output, "==> Downloading package: 25% (25.0 MiB / 100.0 MiB)"));

    reset_mocks();
    mock_progress_total = (curl_off_t)100 * 1024 * 1024;
    mock_progress_downloaded = (curl_off_t)25 * 1024 * 1024;
    build_path(destination, sizeof(destination), "progress-binary.out");

    capture = tmpfile();
    TEST_ASSERT_NOT_NULL(capture);
    fflush(stdout);
    saved_stdout = test_dup_fd(TEST_PLATFORM_STDOUT_FD);
    TEST_ASSERT_TRUE(saved_stdout >= 0);
    TEST_ASSERT_TRUE(
        test_dup2_fd(test_file_descriptor(capture), TEST_PLATFORM_STDOUT_FD) >= 0);
    result = download_file("https://example.invalid/binary",
                           destination,
                           DOWNLOAD_VALIDATE_BINARY);
    fflush(stdout);
    TEST_ASSERT_TRUE(test_dup2_fd(saved_stdout, TEST_PLATFORM_STDOUT_FD) >= 0);
    TEST_ASSERT_EQUAL_INT(0, test_close_fd(saved_stdout));
    TEST_ASSERT_EQUAL_INT(CUP_OK, result);
    TEST_ASSERT_EQUAL_INT(0, fseek(capture, 0, SEEK_SET));
    bytes = fread(output, 1, sizeof(output) - 1, capture);
    output[bytes] = '\0';
    TEST_ASSERT_EQUAL_INT(0, fclose(capture));
    TEST_ASSERT_NOT_NULL(strstr(
        output, "==> Downloading cup: 25% (25.0 MiB / 100.0 MiB)"));

    reset_mocks();
    mock_progress_total = (curl_off_t)100 * 1024 * 1024;
    mock_progress_downloaded = (curl_off_t)25 * 1024 * 1024;
    build_path(destination, sizeof(destination), "progress-metadata.out");

    capture = tmpfile();
    TEST_ASSERT_NOT_NULL(capture);
    fflush(stdout);
    saved_stdout = test_dup_fd(TEST_PLATFORM_STDOUT_FD);
    TEST_ASSERT_TRUE(saved_stdout >= 0);
    TEST_ASSERT_TRUE(
        test_dup2_fd(test_file_descriptor(capture), TEST_PLATFORM_STDOUT_FD) >= 0);
    result = download_file("https://example.invalid/metadata",
                           destination,
                           DOWNLOAD_VALIDATE_METADATA);
    fflush(stdout);
    TEST_ASSERT_TRUE(test_dup2_fd(saved_stdout, TEST_PLATFORM_STDOUT_FD) >= 0);
    TEST_ASSERT_EQUAL_INT(0, test_close_fd(saved_stdout));
    TEST_ASSERT_EQUAL_INT(CUP_OK, result);
    TEST_ASSERT_EQUAL_INT(0, fseek(capture, 0, SEEK_SET));
    bytes = fread(output, 1, sizeof(output) - 1, capture);
    output[bytes] = '\0';
    TEST_ASSERT_EQUAL_INT(0, fclose(capture));
    TEST_ASSERT_EQUAL_STRING("", output);
}

static void assert_download_argument_failures(const char *destination) {
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          download_file(NULL, destination, DOWNLOAD_VALIDATE_METADATA));
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_INVALID_INPUT,
        download_file("https://example.invalid", NULL, DOWNLOAD_VALIDATE_METADATA));
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_INVALID_INPUT,
        download_file("https://example.invalid", destination, (DownloadValidation)999));
}

static void assert_download_setup_failures(const char *destination) {
    char missing_parent[1024];

    reset_mocks();
#if defined(CUP_USE_OPENSSL_INIT)
    mock_tls_init_result = 0;
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_FETCH,
        download_file("https://example.invalid", destination, DOWNLOAD_VALIDATE_METADATA));
#endif

    reset_mocks();
    mock_global_result = CURLE_FAILED_INIT;
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_FETCH,
        download_file("https://example.invalid", destination, DOWNLOAD_VALIDATE_METADATA));

    reset_mocks();
    build_path(missing_parent, sizeof(missing_parent), "missing/child.out");
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_TEMPORARY,
        download_file("https://example.invalid", missing_parent, DOWNLOAD_VALIDATE_METADATA));

    reset_mocks();
    mock_easy_init_null = 1;
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_FETCH,
        download_file("https://example.invalid", destination, DOWNLOAD_VALIDATE_METADATA));

    reset_mocks();
    mock_fail_option = CURLOPT_URL;
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_FETCH,
        download_file("https://example.invalid", destination, DOWNLOAD_VALIDATE_METADATA));

#if defined(CUP_USE_EMBEDDED_CA_BUNDLE)
    reset_mocks();
    mock_fail_option = CURLOPT_CAINFO_BLOB;
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_FETCH,
        download_file("https://example.invalid", destination, DOWNLOAD_VALIDATE_METADATA));
#endif
}

static void assert_download_transport_failures(const char *destination) {
    reset_mocks();
    mock_perform_result = CURLE_OPERATION_TIMEDOUT;
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_TIMEOUT,
        download_file("https://example.invalid", destination, DOWNLOAD_VALIDATE_METADATA));

    reset_mocks();
    mock_perform_result = CURLE_PEER_FAILED_VERIFICATION;
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_TLS,
        download_file("https://example.invalid", destination, DOWNLOAD_VALIDATE_METADATA));

    reset_mocks();
    mock_response_code = 404;
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_FETCH,
        download_file("https://example.invalid", destination, DOWNLOAD_VALIDATE_METADATA));

    reset_mocks();
    mock_interrupt = 1;
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_INTERRUPT,
        download_file("https://example.invalid", destination, DOWNLOAD_VALIDATE_METADATA));

    reset_mocks();
    mock_info_result = CURLE_BAD_FUNCTION_ARGUMENT;
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_FETCH,
        download_file("https://example.invalid", destination, DOWNLOAD_VALIDATE_METADATA));

    reset_mocks();
    mock_perform_result = CURLE_SSL_CONNECT_ERROR;
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_TLS,
        download_file("https://example.invalid", destination, DOWNLOAD_VALIDATE_METADATA));
}

static void assert_download_content_failures(const char *destination) {
    reset_mocks();
    mock_too_large = 1;
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_DOWNLOAD_TOO_LARGE,
        download_file("https://example.invalid", destination, DOWNLOAD_VALIDATE_METADATA));

    TEST_ASSERT_EQUAL_INT((curl_off_t)MAX_METADATA_DOWNLOAD_BYTES, mock_max_filesize);

    reset_mocks();
    write_text(destination, "old\n");
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_INTERRUPT,
        download_file_checked("https://example.invalid",
                              destination,
                              DOWNLOAD_VALIDATE_METADATA,
                              request_interrupt_after_validation,
                              NULL));
    {
        char content[32];
        read_text(destination, content, sizeof(content));
        TEST_ASSERT_EQUAL_STRING("old\n", content);
    }

    reset_mocks();
    mock_payload[0] = '\0';
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_FETCH,
        download_file("https://example.invalid", destination, DOWNLOAD_VALIDATE_METADATA));

    /* Unauthenticated archive bytes receive only raw bounded/non-empty validation here. */
    reset_mocks();
    TEST_ASSERT_EQUAL_INT(
        CUP_OK,
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
        download_file("https://example.invalid", long_path, DOWNLOAD_VALIDATE_METADATA));

    reset_mocks();
    TEST_ASSERT_EQUAL_INT(0, test_mkdir(destination, 0755));
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_FILESYSTEM,
        download_file("https://example.invalid", destination, DOWNLOAD_VALIDATE_METADATA));
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

static PackageArtifactSpec artifact_spec_for(const char *version) {
    PackageArtifactSpec spec;

    memset(&spec, 0, sizeof(spec));
    spec.identity = identity_for(version);
    spec.format = PACKAGE_ARCHIVE_FORMAT_TAR_GZ;
    TEST_ASSERT_TRUE(snprintf(spec.package_url,
                              sizeof(spec.package_url),
                              "https://example.invalid/archive") > 0);
    TEST_ASSERT_TRUE(snprintf(spec.artifact_sha256,
                              sizeof(spec.artifact_sha256),
                              "%s",
                              "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa") > 0);
    return spec;
}

static void test_cache_source_results(void) {
    PackageArtifactSpec spec = artifact_spec_for("22.1.5-typed-cache");
    PackageCacheSource source;
    VerifiedArtifact artifact;
    char archive_path[MAX_PATH_LEN];

    memset(&artifact, 0, sizeof(artifact));
    source = (PackageCacheSource)0x7f;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          package_cache_fetch_artifact(NULL, &spec, &source));
    TEST_ASSERT_EQUAL_INT(PACKAGE_CACHE_SOURCE_NONE, source);
    source = (PackageCacheSource)0x7f;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          package_cache_fetch_artifact(&artifact, NULL, &source));
    TEST_ASSERT_EQUAL_INT(PACKAGE_CACHE_SOURCE_NONE, source);
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          package_cache_fetch_artifact(&artifact, &spec, NULL));

    reset_mocks();
    spec = artifact_spec_for("22.1.5-typed-valid");
    make_cache_file(&spec, archive_path, sizeof(archive_path));
    push_artifact(CUP_OK, ARTIFACT_VERIFY_VALID);
    TEST_ASSERT_EQUAL_INT(CUP_OK,
                          package_cache_fetch_artifact(&artifact, &spec, &source));
    TEST_ASSERT_EQUAL_INT(PACKAGE_CACHE_SOURCE_CACHE, source);
    TEST_ASSERT_FALSE(artifact.disposable);
    TEST_ASSERT_EQUAL_size_t(1, artifact_open_index);

    reset_mocks();
    memset(&artifact, 0, sizeof(artifact));
    spec = artifact_spec_for("22.1.5-typed-stale");
    make_cache_file(&spec, archive_path, sizeof(archive_path));
    push_artifact(CUP_OK, ARTIFACT_VERIFY_DIGEST_MISMATCH);
    push_artifact(CUP_OK, ARTIFACT_VERIFY_VALID);
    TEST_ASSERT_EQUAL_INT(CUP_OK,
                          package_cache_fetch_artifact(&artifact, &spec, &source));
    TEST_ASSERT_EQUAL_INT(PACKAGE_CACHE_SOURCE_NETWORK, source);
    TEST_ASSERT_TRUE(artifact.disposable);
    TEST_ASSERT_EQUAL_size_t(2, artifact_open_index);
    TEST_ASSERT_EQUAL_size_t(1, artifact_discard_calls);
    TEST_ASSERT_TRUE(strstr(mock_url, "archive") != NULL);

    reset_mocks();
    memset(&artifact, 0, sizeof(artifact));
    spec = artifact_spec_for("22.1.5-typed-rejected");
    make_cache_file(&spec, archive_path, sizeof(archive_path));
    push_artifact(CUP_OK, ARTIFACT_VERIFY_REJECTED);
    mock_fail_url = "archive";
    mock_perform_result = CURLE_COULDNT_CONNECT;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FETCH,
                          package_cache_fetch_artifact(&artifact, &spec, &source));
    TEST_ASSERT_EQUAL_INT(PACKAGE_CACHE_SOURCE_NONE, source);
    TEST_ASSERT_EQUAL_size_t(1, artifact_discard_calls);
    TEST_ASSERT_FALSE(test_access_exists(archive_path));

    reset_mocks();
    memset(&artifact, 0, sizeof(artifact));
    spec = artifact_spec_for("22.1.5-typed-wrong-kind");
    make_cache_file(&spec, archive_path, sizeof(archive_path));
    TEST_ASSERT_EQUAL_INT(0, test_unlink(archive_path));
    TEST_ASSERT_EQUAL_INT(0, test_mkdir(archive_path, 0755));
    push_artifact(CUP_OK, ARTIFACT_VERIFY_WRONG_TYPE);
    push_artifact(CUP_OK, ARTIFACT_VERIFY_VALID);
    TEST_ASSERT_EQUAL_INT(CUP_OK,
                          package_cache_fetch_artifact(&artifact, &spec, &source));
    TEST_ASSERT_EQUAL_INT(PACKAGE_CACHE_SOURCE_NETWORK, source);
    TEST_ASSERT_TRUE(artifact.disposable);
    TEST_ASSERT_EQUAL_size_t(0, artifact_discard_calls);
    TEST_ASSERT_TRUE(test_access_exists(archive_path));

    reset_mocks();
    memset(&artifact, 0, sizeof(artifact));
    spec = artifact_spec_for("22.1.5-typed-network");
    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_ensure_cache());
    TEST_ASSERT_EQUAL_INT(CUP_OK,
                          layout_build_cache_path(archive_path,
                                                  sizeof(archive_path),
                                                  spec.artifact_sha256));
    if (test_access_exists(archive_path)) {
        TEST_ASSERT_EQUAL_INT(0, test_remove_tree(archive_path));
    }
    push_artifact(CUP_OK, ARTIFACT_VERIFY_MISSING);
    push_artifact(CUP_OK, ARTIFACT_VERIFY_VALID);
    TEST_ASSERT_EQUAL_INT(CUP_OK,
                          package_cache_fetch_artifact(&artifact, &spec, &source));
    TEST_ASSERT_EQUAL_INT(PACKAGE_CACHE_SOURCE_NETWORK, source);
    TEST_ASSERT_EQUAL_size_t(2, artifact_open_index);

    reset_mocks();
    memset(&artifact, 0, sizeof(artifact));
    spec = artifact_spec_for("22.1.5-typed-network-digest-failure");
    push_artifact(CUP_OK, ARTIFACT_VERIFY_MISSING);
    push_artifact(CUP_OK, ARTIFACT_VERIFY_DIGEST_MISMATCH);
    TEST_ASSERT_EQUAL_INT(CUP_ERR_VALIDATION,
                          package_cache_fetch_artifact(&artifact, &spec, &source));
    TEST_ASSERT_EQUAL_INT(PACKAGE_CACHE_SOURCE_NONE, source);
    TEST_ASSERT_EQUAL_size_t(1, artifact_discard_calls);

    reset_mocks();
    memset(&artifact, 0, sizeof(artifact));
    spec = artifact_spec_for("22.1.5-typed-open-error");
    push_artifact(CUP_ERR_FILESYSTEM, ARTIFACT_VERIFY_NONE);
    push_artifact(CUP_OK, ARTIFACT_VERIFY_VALID);
    TEST_ASSERT_EQUAL_INT(CUP_OK,
                          package_cache_fetch_artifact(&artifact, &spec, &source));
    TEST_ASSERT_EQUAL_INT(PACKAGE_CACHE_SOURCE_NETWORK, source);
    TEST_ASSERT_TRUE(artifact.disposable);

    reset_mocks();
    memset(&artifact, 0, sizeof(artifact));
    spec = artifact_spec_for("22.1.5-typed-discard-error");
    make_cache_file(&spec, archive_path, sizeof(archive_path));
    push_artifact(CUP_OK, ARTIFACT_VERIFY_DIGEST_MISMATCH);
    artifact_discard_result = CUP_ERR_FILESYSTEM;
    push_artifact(CUP_OK, ARTIFACT_VERIFY_VALID);
    TEST_ASSERT_EQUAL_INT(CUP_OK,
                          package_cache_fetch_artifact(&artifact, &spec, &source));
    TEST_ASSERT_EQUAL_INT(PACKAGE_CACHE_SOURCE_NETWORK, source);
    TEST_ASSERT_TRUE(artifact.disposable);
    TEST_ASSERT_EQUAL_size_t(1, artifact_discard_calls);
}



int main(void) {
    TEST_ASSERT_NOT_NULL(test_make_temp_directory(
        temp_home, sizeof(temp_home), "cup-package-cache-test"));
    TEST_ASSERT_EQUAL_INT(0, test_set_home(temp_home));
    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_ensure_runtime());

    UNITY_BEGIN();
    RUN_TEST(test_protocol_policy);
    RUN_TEST(test_file_success);
    RUN_TEST(test_download_progress_output);
    RUN_TEST(test_file_failures);
    RUN_TEST(test_cache_source_results);
    return UNITY_END();
}
