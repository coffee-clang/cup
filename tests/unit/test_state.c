/* Tests the format-2 dynamic state contract and its atomic persistence. */
#include "error.h"
#include "package.h"
#include "state.h"
#include "text.h"
#include "unity.h"
#include "test_platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char temp_dir[CUP_TEST_TEMP_PATH_SIZE];
static CupError state_path_error;

static CupError copy_field(char *buffer, size_t size, const char *value) {
    size_t length;
    if (buffer == NULL || size == 0 || value == NULL || value[0] == '\0') return CUP_ERR_INVALID_INPUT;
    length = strlen(value);
    if (length >= size) return CUP_ERR_BUFFER_TOO_SMALL;
    memcpy(buffer, value, length + 1);
    return CUP_OK;
}

static CupError buffer_write_result(int written, size_t size) {
    return written >= 0 && (size_t)written < size ? CUP_OK : CUP_ERR_BUFFER_TOO_SMALL;
}

void setUp(void) {
    char path[MAX_PATH_LEN];
    state_path_error = CUP_OK;
    if (snprintf(path, sizeof(path), "%s/state.txt", temp_dir) > 0) (void)test_unlink(path);
}
void tearDown(void) {}

CupError platform_get_host(char *buffer, size_t size) { return copy_field(buffer, size, "linux-x64"); }
CupError layout_get_root(char *buffer, size_t size) {
    return buffer_write_result(snprintf(buffer, size, "%s", temp_dir), size);
}
CupError layout_get_state_path(char *buffer, size_t size) {
    if (state_path_error != CUP_OK) return state_path_error;
    return buffer_write_result(snprintf(buffer, size, "%s/state.txt", temp_dir), size);
}

CupError package_scope_init(PackageScope *scope, const char *component,
                            const char *host, const char *target) {
    CupError err;
    if (scope == NULL) return CUP_ERR_INVALID_INPUT;
    memset(scope, 0, sizeof(*scope));
    if ((err = copy_field(scope->component,sizeof(scope->component),component)) != CUP_OK) return err;
    if ((err = copy_field(scope->host_platform,sizeof(scope->host_platform),host)) != CUP_OK) return err;
    return copy_field(scope->target_platform,sizeof(scope->target_platform),target);
}
int package_scope_equals(const PackageScope *a,const PackageScope *b) {
    return a && b && strcmp(a->component,b->component)==0 &&
           strcmp(a->host_platform,b->host_platform)==0 &&
           strcmp(a->target_platform,b->target_platform)==0;
}
CupError package_identity_init(PackageIdentity *id,const char *component,const char *tool,
                               const char *host,const char *target,const char *version) {
    if (id == NULL || component == NULL || tool == NULL || host == NULL || target == NULL || version == NULL ||
        component[0]=='\0'||tool[0]=='\0'||host[0]=='\0'||target[0]=='\0'||version[0]=='\0'||
        strcmp(version,"stable")==0 || strstr(version,"bad") != NULL) return CUP_ERR_VALIDATION;
    memset(id,0,sizeof(*id));
    if (copy_field(id->component,sizeof(id->component),component)!=CUP_OK ||
        copy_field(id->tool,sizeof(id->tool),tool)!=CUP_OK ||
        copy_field(id->host_platform,sizeof(id->host_platform),host)!=CUP_OK ||
        copy_field(id->target_platform,sizeof(id->target_platform),target)!=CUP_OK ||
        copy_field(id->version,sizeof(id->version),version)!=CUP_OK) return CUP_ERR_BUFFER_TOO_SMALL;
    return CUP_OK;
}
CupError package_identity_validate(const PackageIdentity *id, FILE *diagnostics) {
    PackageIdentity tmp; (void)diagnostics;
    if (id == NULL) return CUP_ERR_INVALID_INPUT;
    return package_identity_init(&tmp,id->component,id->tool,id->host_platform,id->target_platform,id->version);
}
CupError package_identity_from_selector(PackageIdentity *id,const char *component,const char *host,
                                        const char *target,const char *selector,FILE *diagnostics) {
    const char *at; char tool[MAX_IDENTIFIER_LEN]; size_t n; (void)diagnostics;
    if (selector == NULL || (at=strchr(selector,'@')) == NULL || at==selector || at[1]=='\0' || strchr(at+1,'@')) return CUP_ERR_VALIDATION;
    n=(size_t)(at-selector); if (n>=sizeof(tool)) return CUP_ERR_BUFFER_TOO_SMALL;
    memcpy(tool,selector,n); tool[n]='\0';
    return package_identity_init(id,component,tool,host,target,at+1);
}
CupError package_identity_get_scope(const PackageIdentity *id, PackageScope *scope) {
    if (id == NULL) return CUP_ERR_INVALID_INPUT;
    return package_scope_init(scope,id->component,id->host_platform,id->target_platform);
}
int package_identity_equals(const PackageIdentity *a,const PackageIdentity *b) {
    return a&&b&&strcmp(a->component,b->component)==0&&strcmp(a->tool,b->tool)==0&&
           strcmp(a->host_platform,b->host_platform)==0&&strcmp(a->target_platform,b->target_platform)==0&&
           strcmp(a->version,b->version)==0;
}
CupError package_identity_format_selector(const PackageIdentity *id,char *buffer,size_t size) {
    int n;
    if (package_identity_validate(id,NULL)!=CUP_OK) return CUP_ERR_VALIDATION;
    n=snprintf(buffer,size,"%s@%s",id->tool,id->version);
    return n>=0&&(size_t)n<size?CUP_OK:CUP_ERR_BUFFER_TOO_SMALL;
}

static PackageIdentity make_identity(const char *tool,const char *target,const char *version) {
    PackageIdentity id;
    TEST_ASSERT_EQUAL_INT(CUP_OK,package_identity_init(&id,"compiler",tool,"linux-x64",target,version));
    return id;
}
static PackageScope make_scope(const char *target) {
    PackageScope s;
    TEST_ASSERT_EQUAL_INT(CUP_OK,package_scope_init(&s,"compiler","linux-x64",target));
    return s;
}
static void state_path(char *path,size_t size) {
    TEST_ASSERT_EQUAL_INT(CUP_OK,layout_get_state_path(path,size));
}
static void write_bytes(const void *data,size_t size) {
    char path[MAX_PATH_LEN]; FILE *f; state_path(path,sizeof(path)); f=fopen(path,"wb");
    TEST_ASSERT_NOT_NULL(f); TEST_ASSERT_EQUAL_size_t(size,fwrite(data,1,size,f)); TEST_ASSERT_EQUAL_INT(0,fclose(f));
}
static void write_text(const char *text) { write_bytes(text,strlen(text)); }
static char *read_text(void) {
    char path[MAX_PATH_LEN]; FILE *f; long n; char *data; state_path(path,sizeof(path));
    f=fopen(path,"rb"); TEST_ASSERT_NOT_NULL(f); TEST_ASSERT_EQUAL_INT(0,fseek(f,0,SEEK_END)); n=ftell(f); TEST_ASSERT_TRUE(n>=0);
    TEST_ASSERT_EQUAL_INT(0,fseek(f,0,SEEK_SET)); data=calloc((size_t)n+1,1); TEST_ASSERT_NOT_NULL(data);
    TEST_ASSERT_EQUAL_size_t((size_t)n,fread(data,1,(size_t)n,f)); TEST_ASSERT_EQUAL_INT(0,fclose(f)); return data;
}

static void test_dynamic_mutation_and_defaults(void) {
    CupState state; PackageIdentity id; PackageScope scope=make_scope("linux-x64"); size_t i;
    state_init(&state);
    for (i=0;i<300u;++i) {
        char version[32]; snprintf(version,sizeof(version),"1.0.%zu",i); id=make_identity("clang","linux-x64",version);
        TEST_ASSERT_EQUAL_INT(CUP_OK,state_add_installed(&state,&id));
    }
    TEST_ASSERT_EQUAL_size_t(300,state.installed_count);
    TEST_ASSERT_TRUE(state.installed_capacity>=300);
    id=make_identity("clang","linux-x64","1.0.299");
    TEST_ASSERT_TRUE(state_find_installed(&state,&id)>=0);
    TEST_ASSERT_EQUAL_INT(CUP_OK,state_set_default(&state,&id));
    TEST_ASSERT_NOT_NULL(state_get_default(&state,&scope));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INCONSISTENT_STATE,state_remove_installed(&state,&id));
    TEST_ASSERT_EQUAL_INT(CUP_OK,state_clear_default(&state,&scope));
    TEST_ASSERT_EQUAL_INT(CUP_OK,state_remove_installed(&state,&id));
    TEST_ASSERT_EQUAL_size_t(299,state.installed_count);
    state_free(&state);
}

static void test_tool_reference_prefers_same_tool_default_otherwise_max(void) {
    CupState state;
    PackageScope scope = make_scope("linux-x64");
    PackageIdentity reference;
    PackageIdentity id;
    int reference_is_default = -1;

    state_init(&state);
    id = make_identity("clang", "linux-x64", "1.0.0");
    TEST_ASSERT_EQUAL_INT(CUP_OK, state_add_installed(&state, &id));
    id = make_identity("clang", "linux-x64", "3.0.0");
    TEST_ASSERT_EQUAL_INT(CUP_OK, state_add_installed(&state, &id));
    id = make_identity("gcc", "linux-x64", "9.0.0");
    TEST_ASSERT_EQUAL_INT(CUP_OK, state_add_installed(&state, &id));
    TEST_ASSERT_EQUAL_INT(CUP_OK, state_set_default(&state, &id));

    TEST_ASSERT_EQUAL_INT(
        CUP_OK, state_get_tool_reference(&state, &scope, "clang", &reference, &reference_is_default));
    TEST_ASSERT_EQUAL_STRING("3.0.0", reference.version);
    TEST_ASSERT_FALSE(reference_is_default);

    id = make_identity("clang", "linux-x64", "1.0.0");
    TEST_ASSERT_EQUAL_INT(CUP_OK, state_set_default(&state, &id));
    TEST_ASSERT_EQUAL_INT(
        CUP_OK, state_get_tool_reference(&state, &scope, "clang", &reference, &reference_is_default));
    TEST_ASSERT_EQUAL_STRING("1.0.0", reference.version);
    TEST_ASSERT_TRUE(reference_is_default);

    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_NOT_INSTALLED,
        state_get_tool_reference(&state, &scope, "lldb", &reference, &reference_is_default));
    state_free(&state);
}

static void test_empty_state_round_trip(void) {
    CupState state, loaded;
    StateFileStatus status;
    SystemPathIdentity identity = {0};

    state_init(&state);
    state_init(&loaded);
    TEST_ASSERT_EQUAL_INT(CUP_OK, state_save(&state, NULL, &identity));
    TEST_ASSERT_TRUE(identity.valid);
    TEST_ASSERT_EQUAL_INT(CUP_OK, state_load(&loaded, &status, NULL, stderr));
    TEST_ASSERT_EQUAL_INT(STATE_FILE_LOADED, status);
    TEST_ASSERT_EQUAL_size_t(0, loaded.installed_count);
    TEST_ASSERT_EQUAL_size_t(0, loaded.default_count);
    state_free(&loaded);
    state_free(&state);
}

static void test_save_load_format2_host_implicit(void) {
    CupState state, loaded; StateFileStatus status; SystemPathIdentity identity={0};
    PackageIdentity a=make_identity("clang","linux-x64","23.1.0");
    PackageIdentity b=make_identity("gcc","windows-x64","16.2.0-rev1");
    char *text;
    state_init(&state); state_init(&loaded);
    TEST_ASSERT_EQUAL_INT(CUP_OK,state_add_installed(&state,&b));
    TEST_ASSERT_EQUAL_INT(CUP_OK,state_add_installed(&state,&a));
    TEST_ASSERT_EQUAL_INT(CUP_OK,state_set_default(&state,&a));
    TEST_ASSERT_EQUAL_INT(CUP_OK,state_save(&state,NULL,&identity)); TEST_ASSERT_TRUE(identity.valid);
    text=read_text();
    TEST_ASSERT_NOT_NULL(strstr(text,"format=2\n"));
    TEST_ASSERT_NOT_NULL(strstr(text,"installed.compiler.linux-x64=clang@23.1.0\n"));
    TEST_ASSERT_NOT_NULL(strstr(text,"installed.compiler.windows-x64=gcc@16.2.0-rev1\n"));
    TEST_ASSERT_NOT_NULL(strstr(text,"default.compiler.linux-x64=clang@23.1.0\n"));
    TEST_ASSERT_NULL(strstr(text,"linux-x64.linux-x64")); free(text);
    TEST_ASSERT_EQUAL_INT(CUP_OK,state_load(&loaded,&status,NULL,stderr));
    TEST_ASSERT_EQUAL_INT(STATE_FILE_LOADED,status); TEST_ASSERT_EQUAL_size_t(2,loaded.installed_count);
    TEST_ASSERT_EQUAL_STRING("linux-x64",loaded.installed[0].host_platform);
    TEST_ASSERT_EQUAL_INT(CUP_OK,state_validate(&loaded,stderr));
    state_free(&loaded); state_free(&state);
}

static void test_load_validates_before_publish(void) {
    CupState state; StateFileStatus status; state_init(&state);
    state_free(&state); state_init(&state);
    write_text("format=2\ndefault.compiler.linux-x64=clang@23.1.0\n");
    TEST_ASSERT_EQUAL_INT(CUP_ERR_STATE_LOAD,state_load(&state,&status,NULL,NULL));
    TEST_ASSERT_EQUAL_size_t(0,state.installed_count); TEST_ASSERT_EQUAL_size_t(0,state.default_count);
    state_free(&state);
}

static void test_rejects_legacy_and_malformed_state(void) {
    CupState state; StateFileStatus status; state_init(&state);
    write_text("format=1\ninstalled.compiler.linux-x64.linux-x64=clang@1\n");
    TEST_ASSERT_EQUAL_INT(CUP_ERR_STATE_LOAD,state_load(&state,&status,NULL,NULL));
    write_text("format=2\ninstalled.compiler.linux-x64.linux-x64=clang@1\n");
    TEST_ASSERT_EQUAL_INT(CUP_ERR_STATE_LOAD,state_load(&state,&status,NULL,NULL));
    write_text("format=2\ninstalled.compiler.linux-x64=bad-entry\n");
    TEST_ASSERT_EQUAL_INT(CUP_ERR_STATE_LOAD,state_load(&state,&status,NULL,NULL));
    write_text("format=2\ninstalled.compiler.linux-x64=clang@1\ninstalled.compiler.linux-x64=clang@1\n");
    TEST_ASSERT_EQUAL_INT(CUP_ERR_STATE_LOAD,state_load(&state,&status,NULL,NULL));
    { static const unsigned char bytes[]="format=2\ninstalled.compiler.linux-x64=clang@1\0\n"; write_bytes(bytes,sizeof(bytes)-1); }
    TEST_ASSERT_EQUAL_INT(CUP_ERR_STATE_LOAD,state_load(&state,&status,NULL,NULL));
    write_bytes("format=2\ninstalled.compiler.linux-x64=clang@1",strlen("format=2\ninstalled.compiler.linux-x64=clang@1"));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_STATE_LOAD,state_load(&state,&status,NULL,NULL));
    state_free(&state);
}

static void test_document_budget_and_missing(void) {
    CupState state; StateFileStatus status; SystemPathIdentity source={0}; char path[MAX_PATH_LEN];
    size_t size=MAX_STATE_FILE_BYTES+1u; char *data=malloc(size); TEST_ASSERT_NOT_NULL(data);
    memset(data,'x',size); data[size-1]='\n'; write_bytes(data,size); free(data); state_init(&state);
    TEST_ASSERT_EQUAL_INT(CUP_ERR_STATE_FULL,state_load(&state,&status,&source,NULL)); TEST_ASSERT_FALSE(source.valid);
    state_path(path,sizeof(path)); TEST_ASSERT_EQUAL_INT(0,test_unlink(path)); source.valid=1;
    TEST_ASSERT_EQUAL_INT(CUP_OK,state_load(&state,&status,&source,NULL)); TEST_ASSERT_EQUAL_INT(STATE_FILE_MISSING,status); TEST_ASSERT_FALSE(source.valid);
    state_free(&state);
}

static void test_host_validation_is_in_memory_only(void) {
    CupState state; PackageIdentity native=make_identity("clang","linux-x64","1"); PackageIdentity foreign;
    state_init(&state); TEST_ASSERT_EQUAL_INT(CUP_OK,state_add_installed(&state,&native));
    TEST_ASSERT_EQUAL_size_t(0,state_count_foreign_hosts(&state,"linux-x64"));
    TEST_ASSERT_EQUAL_INT(CUP_OK,package_identity_init(&foreign,"compiler","clang","windows-x64","linux-x64","2"));
    TEST_ASSERT_EQUAL_INT(CUP_OK,state_add_installed(&state,&foreign));
    TEST_ASSERT_EQUAL_size_t(1,state_count_foreign_hosts(&state,"linux-x64"));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INCONSISTENT_STATE,state_validate_current_host(&state,"linux-x64",NULL));
    state_free(&state);
}



int main(void) {
    int result;
    if (test_make_temp_directory(temp_dir,sizeof(temp_dir),"cup-state-unit-test") == NULL) return 1;
    UNITY_BEGIN();
    RUN_TEST(test_dynamic_mutation_and_defaults);
    RUN_TEST(test_tool_reference_prefers_same_tool_default_otherwise_max);
    RUN_TEST(test_empty_state_round_trip);
    RUN_TEST(test_save_load_format2_host_implicit);
    RUN_TEST(test_load_validates_before_publish);
    RUN_TEST(test_rejects_legacy_and_malformed_state);
    RUN_TEST(test_document_budget_and_missing);
    RUN_TEST(test_host_validation_is_in_memory_only);
    result=UNITY_END(); (void)test_remove_tree(temp_dir); return result;
}
