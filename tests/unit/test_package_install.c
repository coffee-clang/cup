/*
 * Exercises install preparation, cache refresh, commit boundaries, default updates
 * and rollback decisions.
 */

#include "command_context.h"
#include "package_cache.h"
#include "package_install.h"
#include "installed_package.h"
#include "interrupt.h"
#include "package_selector.h"
#include "package_request.h"
#include "package_extract.h"
#include "filesystem.h"
#include "layout.h"
#include "package_catalog.h"
#include "package.h"
#include "state.h"
#include "system.h"
#include "package_transaction.h"
#include "wrappers.h"
#include "unity.h"

#include <stdio.h>
#include <string.h>

#define MAX_STEPS 4

/*
 * Scenario controls and observations. Configured results drive the boundary doubles below;
 * counters record the calls made by production code.
 */

static CupState initial_state;
static CupError parse_result;
static CupError context_result;
static CupError load_state_result;
static CupError load_catalog_result;
static CupError resolve_result;
static CupError identity_result;
static CupError version_result;
static int version_available;
static CupError absent_result;
static CupError valid_installed_result;
static CupError default_format_result;
static CupError format_result;
static int format_supported;
static CupError tmp_result;
static CupError install_path_result;
static CupError begin_result;
static CupError url_result;
static CupError checksum_url_result;
static CupError fetch_results[MAX_STEPS];
static PackageCacheSource fetch_sources[MAX_STEPS];
static CupError extract_results[MAX_STEPS];
static CupError validate_results[MAX_STEPS];
static int interrupt_values[MAX_STEPS];
static CupError safe_point_results[MAX_STEPS];
static CupError discard_result;
static CupError remove_results[MAX_STEPS];
static CupError ensure_dir_result;
static CupError read_only_result;
static CupError parent_result;
static CupError move_results[MAX_STEPS];
static SystemCommitState move_states[MAX_STEPS];
static CupError add_state_result;
static const char *current_default;
static CupError set_default_result;
static CupError plan_build_result;
static CupError save_result;
static CupError clear_results[MAX_STEPS];
static CupError plan_apply_result;
static CupError entry_build_result;
static CupError entry_parse_result;

static int context_end_calls;
static int plan_init_calls;
static int plan_free_calls;
static int plan_build_calls;
static int plan_apply_calls;
static int fetch_calls;
static int extract_calls;
static int validate_calls;
static int interrupt_calls;
static int safe_point_calls;
static int discard_calls;
static int remove_calls;
static int ensure_dir_calls;
static int move_calls;
static int clear_calls;
static int save_calls;

/* Fixture lifecycle and local construction helpers. */

static CupError buffer_write_result(int written, size_t size) {
    return written >= 0 && (size_t)written < size ? CUP_OK : CUP_ERR_BUFFER_TOO_SMALL;
}

static void reset_scenario(void) {
    size_t i;

    /* Request, context, catalog, and package-validation defaults. */
    memset(&initial_state, 0, sizeof(initial_state));
    parse_result = CUP_OK;
    context_result = CUP_OK;
    load_state_result = CUP_OK;
    load_catalog_result = CUP_OK;
    resolve_result = CUP_OK;
    identity_result = CUP_OK;
    version_result = CUP_OK;
    version_available = 1;
    absent_result = CUP_OK;
    valid_installed_result = CUP_OK;
    default_format_result = CUP_OK;
    format_result = CUP_OK;
    format_supported = 1;
    tmp_result = CUP_OK;
    install_path_result = CUP_OK;
    begin_result = CUP_OK;
    url_result = CUP_OK;
    checksum_url_result = CUP_OK;
    discard_result = CUP_OK;
    ensure_dir_result = CUP_OK;
    read_only_result = CUP_OK;
    parent_result = CUP_OK;
    add_state_result = CUP_OK;
    current_default = NULL;
    set_default_result = CUP_OK;
    plan_build_result = CUP_OK;
    save_result = CUP_OK;
    plan_apply_result = CUP_OK;
    entry_build_result = CUP_OK;
    entry_parse_result = CUP_OK;

    /* Side-effect counters observe the transaction without replacing its control flow. */
    context_end_calls = 0;
    plan_init_calls = 0;
    plan_free_calls = 0;
    plan_build_calls = 0;
    plan_apply_calls = 0;
    fetch_calls = 0;
    extract_calls = 0;
    validate_calls = 0;
    interrupt_calls = 0;
    safe_point_calls = 0;
    discard_calls = 0;
    remove_calls = 0;
    ensure_dir_calls = 0;
    move_calls = 0;
    clear_calls = 0;
    save_calls = 0;

    /* Scripted results model successive fetch, extraction, move, and cleanup attempts. */
    for (i = 0; i < MAX_STEPS; ++i) {
        fetch_results[i] = CUP_OK;
        fetch_sources[i] = PACKAGE_CACHE_SOURCE_NETWORK;
        extract_results[i] = CUP_OK;
        validate_results[i] = CUP_OK;
        interrupt_values[i] = 0;
        safe_point_results[i] = CUP_OK;
        remove_results[i] = CUP_OK;
        move_results[i] = CUP_OK;
        move_states[i] = SYSTEM_COMMIT_DURABLE;
        clear_results[i] = CUP_OK;
    }
}

static void add_entry(const char *component,
                      const char *host,
                      const char *target,
                      const char *entry) {
    PackageIdentity *item = &initial_state.installed[initial_state.installed_count++];
    char tool[MAX_IDENTIFIER_LEN];
    char version[MAX_IDENTIFIER_LEN];

    const char *separator = strchr(entry, '@');

    memset(item, 0, sizeof(*item));
    strcpy(item->component, component);
    strcpy(item->host_platform, host);
    strcpy(item->target_platform, target);
    if (separator != NULL) {
        size_t tool_length = (size_t)(separator - entry);
        memcpy(tool, entry, tool_length);
        tool[tool_length] = '\0';
        strcpy(version, separator + 1);
        strcpy(item->tool, tool);
        strcpy(item->version, version);
    }
}

void setUp(void) {
    reset_scenario();
}

void tearDown(void) {
}

/*
 * Controlled boundary doubles. Each implementation exposes one dependency through the scenario
 * state above.
 */

CupError package_request_parse(const char *component, const char *entry, PackageRequest *request) {
    TEST_ASSERT_NOT_NULL(request);
    if (parse_result != CUP_OK) {
        return parse_result;
    }
    TEST_ASSERT_NOT_NULL(component);
    TEST_ASSERT_NOT_NULL(entry);
    memset(request, 0, sizeof(*request));
    strcpy(request->selector.tool, "clang");
    strcpy(request->selector.release, strstr(entry, "stable") != NULL ? "stable" : "22.1.5");
    strcpy(request->input_selector, entry);
    return CUP_OK;
}

CupError command_context_begin(CommandContext *context,
                               const char *target_override,
                               SystemLockMode mode) {
    TEST_ASSERT_NOT_NULL(context);
    TEST_ASSERT_EQUAL_INT(SYSTEM_LOCK_EXCLUSIVE, mode);
    memset(context, 0, sizeof(*context));
    if (context_result != CUP_OK) {
        return context_result;
    }
    context->state = initial_state;
    strcpy(context->host_platform, "linux-x64");
    strcpy(context->target_platform, target_override == NULL ? "linux-x64" : target_override);
    return CUP_OK;
}

void command_context_end(CommandContext *context) {
    TEST_ASSERT_NOT_NULL(context);
    context_end_calls++;
}

CupError command_context_load_state(CommandContext *context) {
    TEST_ASSERT_NOT_NULL(context);
    context->state_identity.valid = 1;
    context->state_identity.kind = SYSTEM_PATH_REGULAR_FILE;
    context->state_identity.volume = 1;
    context->state_identity.object = 1;
    return load_state_result;
}

CupError command_context_load_catalog(CommandContext *context) {
    TEST_ASSERT_NOT_NULL(context);
    return load_catalog_result;
}

CupError package_request_resolve(const PackageCatalog *catalog,
                                 const char *component,
                                 const char *host_platform,
                                 const char *target_platform,
                                 PackageRequest *request) {
    (void)catalog;
    TEST_ASSERT_NOT_NULL(component);
    TEST_ASSERT_NOT_NULL(host_platform);
    TEST_ASSERT_NOT_NULL(target_platform);
    TEST_ASSERT_NOT_NULL(request);
    if (resolve_result != CUP_OK) {
        return resolve_result;
    }
    strcpy(request->resolved_release, "22.1.5");
    strcpy(request->resolved_selector, "clang@22.1.5");
    return CUP_OK;
}

CupError package_identity_init(PackageIdentity *identity,
                               const char *component,
                               const char *tool,
                               const char *host_platform,
                               const char *target_platform,
                               const char *version) {
    TEST_ASSERT_NOT_NULL(identity);
    if (identity_result != CUP_OK) {
        return identity_result;
    }
    memset(identity, 0, sizeof(*identity));
    strcpy(identity->component, component);
    strcpy(identity->tool, tool);
    strcpy(identity->host_platform, host_platform);
    strcpy(identity->target_platform, target_platform);
    strcpy(identity->version, version);
    return CUP_OK;
}

CupError package_catalog_has_version(const PackageCatalog *catalog,
                                     const char *component,
                                     const char *tool,
                                     const char *host_platform,
                                     const char *target_platform,
                                     const char *version,
                                     int *is_available) {
    (void)catalog;
    TEST_ASSERT_NOT_NULL(component);
    TEST_ASSERT_NOT_NULL(tool);
    TEST_ASSERT_NOT_NULL(host_platform);
    TEST_ASSERT_NOT_NULL(target_platform);
    TEST_ASSERT_NOT_NULL(version);
    TEST_ASSERT_NOT_NULL(is_available);
    *is_available = version_available;
    return version_result;
}

CupError installed_package_require_absent(const CupState *state, const PackageIdentity *package) {
    TEST_ASSERT_NOT_NULL(state);
    TEST_ASSERT_NOT_NULL(package);
    return absent_result;
}

CupError installed_package_require_valid(const CupState *state, const PackageIdentity *package) {
    TEST_ASSERT_NOT_NULL(state);
    TEST_ASSERT_NOT_NULL(package);
    return valid_installed_result;
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
    if (default_format_result != CUP_OK) {
        return default_format_result;
    }
    return buffer_write_result(snprintf(buffer, size, "tar.gz"), size);
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
    TEST_ASSERT_NOT_NULL(format);
    TEST_ASSERT_NOT_NULL(is_supported);
    *is_supported = format_supported;
    return format_result;
}

CupError layout_create_staging_dir(char *buffer,
                                   size_t size,
                                   const char *operation,
                                   const PackageIdentity *identity) {
    TEST_ASSERT_TRUE(strcmp(operation, "install") == 0 || strcmp(operation, "update") == 0);
    TEST_ASSERT_NOT_NULL(identity);
    if (tmp_result != CUP_OK) {
        return tmp_result;
    }
    return buffer_write_result(snprintf(buffer, size, "/tmp/staging"), size);
}

CupError layout_build_install_path(char *buffer, size_t size, const PackageIdentity *identity) {
    TEST_ASSERT_NOT_NULL(identity);
    if (install_path_result != CUP_OK) {
        return install_path_result;
    }
    return buffer_write_result(snprintf(buffer, size, "/tmp/install"), size);
}

CupError package_transaction_begin(PackageOperation operation,
                                   const PackageIdentity *package,
                                   const char *temporary_path,
                                   PackageTransaction *created) {
    TEST_ASSERT_TRUE(operation == PACKAGE_OPERATION_INSTALL ||
                     operation == PACKAGE_OPERATION_UPDATE);
    TEST_ASSERT_NOT_NULL(package);
    TEST_ASSERT_EQUAL_STRING("/tmp/staging", temporary_path);
    TEST_ASSERT_NOT_NULL(created);

    memset(created, 0, sizeof(*created));
    if (begin_result == CUP_OK || begin_result == CUP_ERR_COMMIT) {
        created->file_identity.valid = 1;
        created->file_identity.kind = SYSTEM_PATH_REGULAR_FILE;
    }
    return begin_result;
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
    (void)tool;
    (void)host_platform;
    (void)target_platform;
    (void)version;
    (void)format;
    if (url_result != CUP_OK) {
        return url_result;
    }
    return buffer_write_result(snprintf(buffer, size, "https://example.invalid/package"), size);
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
    if (checksum_url_result != CUP_OK) {
        return checksum_url_result;
    }
    return buffer_write_result(snprintf(buffer, size, "https://example.invalid/SHA256SUMS"), size);
}

CupError package_artifact_spec_build(PackageArtifactSpec *spec,
                                     const PackageCatalog *catalog,
                                     const PackageIdentity *identity,
                                     const char *format_name) {
    CupError err;

    TEST_ASSERT_NOT_NULL(spec);
    TEST_ASSERT_NOT_NULL(identity);
    TEST_ASSERT_NOT_NULL(format_name);
    memset(spec, 0, sizeof(*spec));
    spec->identity = *identity;
    spec->format = strcmp(format_name, "tar.gz") == 0
                       ? PACKAGE_ARCHIVE_FORMAT_TAR_GZ
                       : PACKAGE_ARCHIVE_FORMAT_TAR_XZ;
    err = package_catalog_build_url(catalog,
                                    spec->package_url,
                                    sizeof(spec->package_url),
                                    identity->component,
                                    identity->tool,
                                    identity->host_platform,
                                    identity->target_platform,
                                    identity->version,
                                    format_name);
    if (err == CUP_OK) {
        err = package_catalog_build_checksum_url(catalog,
                                                 spec->checksum_url,
                                                 sizeof(spec->checksum_url),
                                                 identity->component,
                                                 identity->tool,
                                                 identity->host_platform,
                                                 identity->target_platform,
                                                 identity->version);
    }
    return err;
}

void verified_artifact_init(VerifiedArtifact *artifact) {
    TEST_ASSERT_NOT_NULL(artifact);
    memset(artifact, 0, sizeof(*artifact));
}

void verified_artifact_release(VerifiedArtifact *artifact) {
    TEST_ASSERT_NOT_NULL(artifact);
    memset(artifact, 0, sizeof(*artifact));
}

CupError package_cache_fetch_artifact(VerifiedArtifact *artifact,
                                      const PackageArtifactSpec *spec,
                                      PackageCachePolicy cache_policy,
                                      PackageCacheResult *result) {
    int index = fetch_calls++;

    TEST_ASSERT_TRUE(index < MAX_STEPS);
    TEST_ASSERT_NOT_NULL(artifact);
    TEST_ASSERT_NOT_NULL(spec);
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_EQUAL_STRING("https://example.invalid/package", spec->package_url);
    TEST_ASSERT_EQUAL_STRING("https://example.invalid/SHA256SUMS", spec->checksum_url);
    TEST_ASSERT_EQUAL_INT(index == 0 ? PACKAGE_CACHE_ALLOW : PACKAGE_CACHE_REFRESH, cache_policy);
    if (fetch_results[index] != CUP_OK) {
        return fetch_results[index];
    }
    memset(result, 0, sizeof(*result));
    result->source = fetch_sources[index];
    artifact->file = (FILE *)(uintptr_t)1;
    strcpy(artifact->path, "/tmp/archive.tar.gz");
    artifact->identity.valid = 1;
    artifact->identity.kind = SYSTEM_PATH_REGULAR_FILE;
    return CUP_OK;
}

CupError package_extract_verified(VerifiedArtifact *artifact, const char *tmp_path) {
    int index = extract_calls++;

    TEST_ASSERT_NOT_NULL(artifact);
    TEST_ASSERT_EQUAL_STRING("/tmp/staging", tmp_path);
    TEST_ASSERT_TRUE(index < MAX_STEPS);
    return extract_results[index];
}

CupError verified_artifact_discard(VerifiedArtifact *artifact) {
    TEST_ASSERT_NOT_NULL(artifact);
    discard_calls++;
    if (discard_result == CUP_OK) {
        memset(artifact, 0, sizeof(*artifact));
    }
    return discard_result;
}

CupError package_cache_fetch(char *archive_path,
                             size_t archive_path_size,
                             const char *package_url,
                             const char *checksum_url,
                             const PackageIdentity *identity,
                             const char *format,
                             PackageCachePolicy cache_policy,
                             PackageCacheSource *source) {
    int index = fetch_calls++;
    TEST_ASSERT_TRUE(index < MAX_STEPS);
    TEST_ASSERT_NOT_NULL(package_url);
    TEST_ASSERT_NOT_NULL(checksum_url);
    TEST_ASSERT_NOT_NULL(identity);
    TEST_ASSERT_NOT_NULL(format);
    TEST_ASSERT_NOT_NULL(source);
    TEST_ASSERT_EQUAL_INT(index == 0 ? PACKAGE_CACHE_ALLOW : PACKAGE_CACHE_REFRESH, cache_policy);
    if (fetch_results[index] != CUP_OK) {
        return fetch_results[index];
    }
    snprintf(archive_path, archive_path_size, "/tmp/archive.tar.gz");
    *source = fetch_sources[index];
    return CUP_OK;
}

int interrupt_requested(void) {
    int index = interrupt_calls++;
    TEST_ASSERT_TRUE(index < MAX_STEPS);
    return interrupt_values[index];
}

CupError interrupt_safe_point(void) {
    int index = safe_point_calls++;

    TEST_ASSERT_TRUE(index < MAX_STEPS);
    return safe_point_results[index];
}




CupError package_validate(const char *base_path,
                          const PackageIdentity *identity,
                          FILE *diagnostics) {
    (void)diagnostics;
    int index = validate_calls++;
    TEST_ASSERT_EQUAL_STRING("/tmp/staging", base_path);
    TEST_ASSERT_NOT_NULL(identity);
    TEST_ASSERT_TRUE(index < MAX_STEPS);
    return validate_results[index];
}

CupError filesystem_remove_tree(const char *path) {
    int index = remove_calls++;
    TEST_ASSERT_EQUAL_STRING("/tmp/staging", path);
    TEST_ASSERT_TRUE(index < MAX_STEPS);
    return remove_results[index];
}

CupError filesystem_ensure_directory(const char *path) {
    TEST_ASSERT_EQUAL_STRING("/tmp/staging", path);
    ensure_dir_calls++;
    return ensure_dir_result;
}

CupError package_set_metadata_read_only(const char *base_path) {
    TEST_ASSERT_EQUAL_STRING("/tmp/staging", base_path);
    return read_only_result;
}

CupError layout_ensure_package_parent(const PackageIdentity *identity) {
    TEST_ASSERT_NOT_NULL(identity);
    return parent_result;
}

CupError system_move_path(const char *source,
                          const char *destination,
                          SystemCommitState *commit_state) {
    int index = move_calls++;
    TEST_ASSERT_TRUE(index < MAX_STEPS);
    TEST_ASSERT_NOT_NULL(source);
    TEST_ASSERT_NOT_NULL(destination);
    TEST_ASSERT_NOT_NULL(commit_state);
    *commit_state = move_states[index];
    return move_results[index];
}

CupError state_add_installed(CupState *state, const PackageIdentity *identity) {
    TEST_ASSERT_NOT_NULL(state);
    TEST_ASSERT_NOT_NULL(identity);
    TEST_ASSERT_EQUAL_STRING("compiler", identity->component);
    TEST_ASSERT_EQUAL_STRING("clang", identity->tool);
    TEST_ASSERT_EQUAL_STRING("22.1.5", identity->version);
    return add_state_result;
}

const PackageIdentity *state_get_default(const CupState *state, const PackageScope *scope) {
    static PackageIdentity identity;
    char tool[MAX_IDENTIFIER_LEN];
    char version[MAX_IDENTIFIER_LEN];

    TEST_ASSERT_NOT_NULL(state);
    TEST_ASSERT_NOT_NULL(scope);
    if (current_default == NULL) {
        return NULL;
    }
    TEST_ASSERT_EQUAL_INT(
        CUP_OK,
        package_selector_parse_parts(
            current_default, tool, sizeof(tool), version, sizeof(version)));
    memset(&identity, 0, sizeof(identity));
    strcpy(identity.component, scope->component);
    strcpy(identity.tool, tool);
    strcpy(identity.host_platform, scope->host_platform);
    strcpy(identity.target_platform, scope->target_platform);
    strcpy(identity.version, version);
    return &identity;
}

CupError state_set_default(CupState *state, const PackageIdentity *identity) {
    TEST_ASSERT_NOT_NULL(state);
    TEST_ASSERT_NOT_NULL(identity);
    TEST_ASSERT_EQUAL_STRING("compiler", identity->component);
    TEST_ASSERT_EQUAL_STRING("clang", identity->tool);
    TEST_ASSERT_EQUAL_STRING("22.1.5", identity->version);
    return set_default_result;
}

CupError package_identity_validate(const PackageIdentity *identity, FILE *diagnostics) {
    (void)diagnostics;
    return identity == NULL || identity->tool[0] == '\0' || identity->version[0] == '\0'
               ? CUP_ERR_INVALID_INPUT
               : CUP_OK;
}

CupError package_identity_get_scope(const PackageIdentity *identity, PackageScope *scope) {
    TEST_ASSERT_NOT_NULL(identity);
    TEST_ASSERT_NOT_NULL(scope);
    memset(scope, 0, sizeof(*scope));
    strcpy(scope->component, identity->component);
    strcpy(scope->host_platform, identity->host_platform);
    strcpy(scope->target_platform, identity->target_platform);
    return CUP_OK;
}

CupError package_identity_format_selector(const PackageIdentity *identity,
                                          char *buffer,
                                          size_t size) {
    TEST_ASSERT_NOT_NULL(identity);
    return package_selector_format_parts(buffer, size, identity->tool, identity->version);
}

int package_identity_equals(const PackageIdentity *left, const PackageIdentity *right) {
    return left != NULL && right != NULL && strcmp(left->component, right->component) == 0 &&
           strcmp(left->tool, right->tool) == 0 &&
           strcmp(left->host_platform, right->host_platform) == 0 &&
           strcmp(left->target_platform, right->target_platform) == 0 &&
           strcmp(left->version, right->version) == 0;
}

CupError wrapper_plan_build(WrapperPlan *plan, const CupState *state) {
    TEST_ASSERT_NOT_NULL(plan);
    TEST_ASSERT_NOT_NULL(state);
    plan_build_calls++;
    return plan_build_result;
}

CupError state_save(const CupState *state,
                    const SystemPathIdentity *expected_identity,
                    SystemPathIdentity *published_identity) {
    TEST_ASSERT_NOT_NULL(state);
    TEST_ASSERT_NOT_NULL(expected_identity);
    TEST_ASSERT_TRUE(expected_identity->valid);
    TEST_ASSERT_EQUAL_INT(SYSTEM_PATH_REGULAR_FILE, expected_identity->kind);
    if (published_identity != NULL) {
        *published_identity = *expected_identity;
    }
    save_calls++;
    return save_result;
}



CupError runtime_journal_clear_if_identity(const SystemPathIdentity *expected_identity) {
    TEST_ASSERT_NOT_NULL(expected_identity);
    TEST_ASSERT_TRUE(expected_identity->valid);
    TEST_ASSERT_EQUAL_INT(SYSTEM_PATH_REGULAR_FILE, expected_identity->kind);
    int index = clear_calls++;
    TEST_ASSERT_TRUE(index < MAX_STEPS);
    return clear_results[index];
}

CupError wrapper_plan_apply(const WrapperPlan *plan) {
    TEST_ASSERT_NOT_NULL(plan);
    plan_apply_calls++;
    return plan_apply_result;
}

void wrapper_plan_init(WrapperPlan *plan) {
    TEST_ASSERT_NOT_NULL(plan);
    memset(plan, 0, sizeof(*plan));
    plan_init_calls++;
}

void wrapper_plan_free(WrapperPlan *plan) {
    TEST_ASSERT_NOT_NULL(plan);
    plan_free_calls++;
}

void package_request_print(FILE *stream, const PackageRequest *request) {
    TEST_ASSERT_NOT_NULL(stream);
    TEST_ASSERT_NOT_NULL(request);
    fputs(request->resolved_selector, stream);
}

CupError package_selector_format_parts(char *buffer,
                                       size_t size,
                                       const char *tool,
                                       const char *release) {
    if (entry_build_result != CUP_OK) {
        return entry_build_result;
    }
    return buffer_write_result(snprintf(buffer, size, "%s@%s", tool, release), size);
}

CupError package_selector_parse_parts(
    const char *entry, char *tool, size_t tool_size, char *release, size_t release_size) {
    const char *separator;
    size_t length;

    if (entry_parse_result != CUP_OK) {
        return entry_parse_result;
    }
    separator = strchr(entry, '@');
    if (separator == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    length = (size_t)(separator - entry);
    if (length >= tool_size || strlen(separator + 1) >= release_size) {
        return CUP_ERR_BUFFER_TOO_SMALL;
    }
    memcpy(tool, entry, length);
    tool[length] = '\0';
    strcpy(release, separator + 1);
    return CUP_OK;
}

static void assert_cleanup(void) {
    TEST_ASSERT_EQUAL_INT(1, context_end_calls);
    TEST_ASSERT_EQUAL_INT(1, plan_init_calls);
    TEST_ASSERT_EQUAL_INT(1, plan_free_calls);
}

/*
 * Test cases exercise the real production entry point while changing only controlled boundary
 * outcomes.
 */

static PackageArtifactSpec test_artifact_spec(void) {
    PackageArtifactSpec spec;

    memset(&spec, 0, sizeof(spec));
    strcpy(spec.identity.component, "compiler");
    strcpy(spec.identity.tool, "clang");
    strcpy(spec.identity.host_platform, "linux-x64");
    strcpy(spec.identity.target_platform, "linux-x64");
    strcpy(spec.identity.version, "22.1.5");
    spec.format = PACKAGE_ARCHIVE_FORMAT_TAR_GZ;
    strcpy(spec.package_url, "https://example.invalid/package");
    strcpy(spec.checksum_url, "https://example.invalid/SHA256SUMS");
    return spec;
}

static CupError install_test_artifact(void) {
    PackageArtifactSpec spec = test_artifact_spec();

    return package_install_artifact(&spec);
}

static CupError update_test_artifact(const char *expected_default_selector,
                                     int *installed,
                                     int *moved) {
    PackageArtifactSpec spec = test_artifact_spec();
    PackageIdentity expected_default;
    const PackageIdentity *expected_default_ptr = NULL;

    if (expected_default_selector != NULL) {
        char tool[MAX_IDENTIFIER_LEN];
        char version[MAX_IDENTIFIER_LEN];
        CupError err = package_selector_parse_parts(expected_default_selector,
                                                    tool,
                                                    sizeof(tool),
                                                    version,
                                                    sizeof(version));
        if (err != CUP_OK) {
            return err;
        }
        err = package_identity_init(&expected_default,
                                    spec.identity.component,
                                    tool,
                                    spec.identity.host_platform,
                                    spec.identity.target_platform,
                                    version);
        if (err != CUP_OK) {
            return err;
        }
        expected_default_ptr = &expected_default;
    }

    return package_install_update_artifact(&spec, expected_default_ptr, installed, moved);
}

static void test_public_inputs(void) {
    PackageArtifactSpec spec = test_artifact_spec();
    int installed;
    int moved;

    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, package_install_artifact(NULL));
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_INVALID_INPUT,
        package_install_update_artifact(NULL, NULL, &installed, &moved));
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_INVALID_INPUT,
        package_install_update_artifact(&spec, NULL, NULL, &moved));

    reset_scenario();
    {
        PackageIdentity invalid_default;

        memset(&invalid_default, 0, sizeof(invalid_default));
        TEST_ASSERT_EQUAL_INT(
            CUP_ERR_INVALID_INPUT,
            package_install_update_artifact(&spec, &invalid_default, &installed, &moved));
    }
}

static void test_prepare_failures(void) {
    PackageArtifactSpec spec;

    context_result = CUP_ERR_LOCK;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_LOCK, install_test_artifact());
    assert_cleanup();

    reset_scenario();
    context_result = CUP_ERR_TRANSACTION;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_TRANSACTION, install_test_artifact());

    reset_scenario();
    load_state_result = CUP_ERR_STATE_LOAD;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_STATE_LOAD, install_test_artifact());

    reset_scenario();
    spec = test_artifact_spec();
    strcpy(spec.identity.host_platform, "macos-x64");
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INCONSISTENT_STATE, package_install_artifact(&spec));

    reset_scenario();
    absent_result = CUP_ERR_INCONSISTENT_STATE;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INCONSISTENT_STATE, install_test_artifact());

    reset_scenario();
    absent_result = CUP_ERR_ALREADY_INSTALLED;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_ALREADY_INSTALLED, install_test_artifact());
}

static void test_update_guards(void) {
    int installed = -1;
    int moved = -1;

    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_NOT_INSTALLED,
        update_test_artifact(NULL, &installed, &moved));
    TEST_ASSERT_EQUAL_INT(0, installed);
    TEST_ASSERT_EQUAL_INT(0, moved);

    reset_scenario();
    add_entry("compiler", "linux-x64", "linux-x64", "broken");
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_INCONSISTENT_STATE,
        update_test_artifact(NULL, &installed, &moved));

    reset_scenario();
    add_entry("compiler", "linux-x64", "linux-x64", "clang@1.0.0");
    absent_result = CUP_ERR_ALREADY_INSTALLED;
    valid_installed_result = CUP_ERR_VALIDATION;
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_VALIDATION,
        update_test_artifact(NULL, &installed, &moved));
}

static void test_transaction_preparation(void) {
    tmp_result = CUP_ERR_TEMPORARY;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_TEMPORARY, install_test_artifact());

    reset_scenario();
    install_path_result = CUP_ERR_BUFFER_TOO_SMALL;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_BUFFER_TOO_SMALL, install_test_artifact());
    TEST_ASSERT_EQUAL_INT(1, remove_calls);

    reset_scenario();
    install_path_result = CUP_ERR_BUFFER_TOO_SMALL;
    remove_results[0] = CUP_ERR_FILESYSTEM;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_ROLLBACK, install_test_artifact());

    reset_scenario();
    begin_result = CUP_ERR_COMMIT;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_COMMIT, install_test_artifact());
    TEST_ASSERT_EQUAL_INT(1, remove_calls);
    TEST_ASSERT_EQUAL_INT(0, clear_calls);
}

static void test_cache_refresh(void) {
    fetch_sources[0] = PACKAGE_CACHE_SOURCE_CACHE;
    extract_results[0] = CUP_ERR_ARCHIVE;

    TEST_ASSERT_EQUAL_INT(CUP_OK, install_test_artifact());
    TEST_ASSERT_EQUAL_INT(2, fetch_calls);
    TEST_ASSERT_EQUAL_INT(2, extract_calls);
    TEST_ASSERT_EQUAL_INT(1, discard_calls);
    TEST_ASSERT_EQUAL_INT(1, remove_calls);
    TEST_ASSERT_EQUAL_INT(1, ensure_dir_calls);

    reset_scenario();
    fetch_sources[0] = PACKAGE_CACHE_SOURCE_CACHE;
    extract_results[0] = CUP_ERR_ARCHIVE;
    discard_result = CUP_ERR_FILESYSTEM;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM, install_test_artifact());

    reset_scenario();
    fetch_sources[0] = PACKAGE_CACHE_SOURCE_CACHE;
    extract_results[0] = CUP_ERR_ARCHIVE;
    remove_results[0] = CUP_ERR_FILESYSTEM;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM, install_test_artifact());

    reset_scenario();
    fetch_sources[0] = PACKAGE_CACHE_SOURCE_CACHE;
    extract_results[0] = CUP_ERR_ARCHIVE;
    ensure_dir_result = CUP_ERR_FILESYSTEM;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM, install_test_artifact());

    reset_scenario();
    validate_results[0] = CUP_ERR_VALIDATION;
    discard_result = CUP_ERR_FILESYSTEM;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM, install_test_artifact());
    TEST_ASSERT_EQUAL_INT(1, discard_calls);
}

static void test_interrupt_safe_points(void) {
    safe_point_results[0] = CUP_ERR_INTERRUPT;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INTERRUPT, install_test_artifact());
    TEST_ASSERT_EQUAL_INT(1, safe_point_calls);
    TEST_ASSERT_EQUAL_INT(0, remove_calls);
    TEST_ASSERT_EQUAL_INT(0, clear_calls);

    reset_scenario();
    safe_point_results[1] = CUP_ERR_INTERRUPT;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INTERRUPT, install_test_artifact());
    TEST_ASSERT_EQUAL_INT(2, safe_point_calls);
    TEST_ASSERT_EQUAL_INT(1, remove_calls);
    TEST_ASSERT_EQUAL_INT(0, clear_calls);

    reset_scenario();
    safe_point_results[2] = CUP_ERR_INTERRUPT;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INTERRUPT, install_test_artifact());
    TEST_ASSERT_EQUAL_INT(3, safe_point_calls);
    TEST_ASSERT_EQUAL_INT(0, move_calls);
    TEST_ASSERT_EQUAL_INT(1, remove_calls);
    TEST_ASSERT_EQUAL_INT(1, clear_calls);
}

static void test_existing_update_honors_persistence_safe_point(void) {
    int installed = 0;
    int moved = 0;

    add_entry("compiler", "linux-x64", "linux-x64", "clang@1.0.0");
    absent_result = CUP_ERR_ALREADY_INSTALLED;
    current_default = "clang@1.0.0";
    safe_point_results[0] = CUP_ERR_INTERRUPT;

    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_INTERRUPT,
        update_test_artifact("clang@1.0.0", &installed, &moved));
    TEST_ASSERT_EQUAL_INT(1, safe_point_calls);
    TEST_ASSERT_EQUAL_INT(0, save_calls);
    TEST_ASSERT_EQUAL_INT(0, plan_apply_calls);
    TEST_ASSERT_EQUAL_INT(0, installed);
    TEST_ASSERT_EQUAL_INT(0, moved);
}

static void test_fetch_failures(void) {
    fetch_results[0] = CUP_ERR_FETCH;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FETCH, install_test_artifact());

    reset_scenario();
    fetch_results[0] = CUP_ERR_COMMIT;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_COMMIT, install_test_artifact());
    TEST_ASSERT_EQUAL_INT(0, move_calls);
    TEST_ASSERT_EQUAL_INT(1, remove_calls);
    TEST_ASSERT_EQUAL_INT(0, clear_calls);

    reset_scenario();
    interrupt_values[0] = 1;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INTERRUPT, install_test_artifact());

    reset_scenario();
    interrupt_values[1] = 1;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INTERRUPT, install_test_artifact());

    reset_scenario();
    read_only_result = CUP_ERR_FILESYSTEM;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM, install_test_artifact());

    reset_scenario();
    parent_result = CUP_ERR_FILESYSTEM;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM, install_test_artifact());
}

static void test_new_install_commit(void) {
    int installed = 0;
    int moved = 0;

    TEST_ASSERT_EQUAL_INT(CUP_OK, install_test_artifact());
    TEST_ASSERT_EQUAL_INT(1, move_calls);
    TEST_ASSERT_EQUAL_INT(1, save_calls);
    TEST_ASSERT_EQUAL_INT(1, clear_calls);
    TEST_ASSERT_EQUAL_INT(1, plan_build_calls);
    TEST_ASSERT_EQUAL_INT(1, plan_apply_calls);

    reset_scenario();
    add_entry("compiler", "linux-x64", "linux-x64", "clang@1.0.0");
    absent_result = CUP_ERR_ALREADY_INSTALLED;
    current_default = "clang@1.0.0";
    TEST_ASSERT_EQUAL_INT(
        CUP_OK,
        update_test_artifact("clang@1.0.0", &installed, &moved));
    TEST_ASSERT_EQUAL_INT(0, installed);
    TEST_ASSERT_EQUAL_INT(1, moved);
    TEST_ASSERT_EQUAL_INT(0, fetch_calls);
    TEST_ASSERT_EQUAL_INT(1, save_calls);
    TEST_ASSERT_EQUAL_INT(1, plan_apply_calls);
}

static void test_commit_failures(void) {
    move_results[0] = CUP_ERR_FILESYSTEM;
    move_states[0] = SYSTEM_COMMIT_NOT_APPLIED;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM, install_test_artifact());
    TEST_ASSERT_EQUAL_INT(1, remove_calls);
    TEST_ASSERT_EQUAL_INT(1, clear_calls);

    reset_scenario();
    move_results[0] = CUP_ERR_FILESYSTEM;
    move_states[0] = SYSTEM_COMMIT_APPLIED;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_COMMIT, install_test_artifact());
    TEST_ASSERT_EQUAL_INT(0, remove_calls);

    reset_scenario();
    add_state_result = CUP_ERR_STATE_FULL;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_STATE_FULL, install_test_artifact());
    TEST_ASSERT_EQUAL_INT(2, move_calls);

    reset_scenario();
    set_default_result = CUP_ERR_DEFAULT_FULL;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_DEFAULT_FULL, install_test_artifact());

    reset_scenario();
    plan_build_result = CUP_ERR_VALIDATION;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_VALIDATION, install_test_artifact());

    reset_scenario();
    save_result = CUP_ERR_FILESYSTEM;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM, install_test_artifact());
    TEST_ASSERT_EQUAL_INT(2, move_calls);

    reset_scenario();
    save_result = CUP_ERR_TRANSACTION;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_TRANSACTION, install_test_artifact());
    TEST_ASSERT_EQUAL_INT(2, move_calls);

    reset_scenario();
    save_result = CUP_ERR_COMMIT;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_COMMIT, install_test_artifact());
    TEST_ASSERT_EQUAL_INT(1, move_calls);

    reset_scenario();
    clear_results[0] = CUP_ERR_FILESYSTEM;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_COMMIT, install_test_artifact());

    reset_scenario();
    plan_apply_result = CUP_ERR_FILESYSTEM;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_COMMIT, install_test_artifact());

    reset_scenario();
    add_entry("compiler", "linux-x64", "linux-x64", "clang@1.0.0");
    absent_result = CUP_ERR_ALREADY_INSTALLED;
    current_default = "clang@1.0.0";
    set_default_result = CUP_ERR_DEFAULT_FULL;
    {
        int installed;
        int moved;

        TEST_ASSERT_EQUAL_INT(
            CUP_ERR_DEFAULT_FULL,
            update_test_artifact("clang@1.0.0", &installed, &moved));
    }

    reset_scenario();
    add_entry("compiler", "linux-x64", "linux-x64", "clang@1.0.0");
    absent_result = CUP_ERR_ALREADY_INSTALLED;
    current_default = "clang@1.0.0";
    save_result = CUP_ERR_FILESYSTEM;
    {
        int installed;
        int moved;

        TEST_ASSERT_EQUAL_INT(
            CUP_ERR_FILESYSTEM,
            update_test_artifact("clang@1.0.0", &installed, &moved));
    }

    reset_scenario();
    add_entry("compiler", "linux-x64", "linux-x64", "clang@1.0.0");
    absent_result = CUP_ERR_ALREADY_INSTALLED;
    current_default = "clang@1.0.0";
    plan_apply_result = CUP_ERR_FILESYSTEM;
    {
        int installed;
        int moved;

        TEST_ASSERT_EQUAL_INT(
            CUP_ERR_COMMIT,
            update_test_artifact("clang@1.0.0", &installed, &moved));
    }
}

static void test_rollback_failures(void) {
    add_state_result = CUP_ERR_STATE_FULL;
    move_results[1] = CUP_ERR_FILESYSTEM;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_ROLLBACK, install_test_artifact());

    reset_scenario();
    extract_results[0] = CUP_ERR_EXTRACT;
    remove_results[0] = CUP_ERR_FILESYSTEM;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_ROLLBACK, install_test_artifact());

    reset_scenario();
    safe_point_results[2] = CUP_ERR_INTERRUPT;
    clear_results[0] = CUP_ERR_FILESYSTEM;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_ROLLBACK, install_test_artifact());
    TEST_ASSERT_EQUAL_INT(0, move_calls);
    TEST_ASSERT_EQUAL_INT(1, remove_calls);
    TEST_ASSERT_EQUAL_INT(1, clear_calls);
}


int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_public_inputs);
    RUN_TEST(test_prepare_failures);
    RUN_TEST(test_update_guards);
    RUN_TEST(test_transaction_preparation);
    RUN_TEST(test_cache_refresh);
    RUN_TEST(test_interrupt_safe_points);
    RUN_TEST(test_existing_update_honors_persistence_safe_point);
    RUN_TEST(test_fetch_failures);
    RUN_TEST(test_new_install_commit);
    RUN_TEST(test_commit_failures);
    RUN_TEST(test_rollback_failures);
    return UNITY_END();
}
