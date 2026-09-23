/* Exercises repair ordering and conservative evidence handling through mocked owner boundaries. */

#include "checksum.h"
#include "commands.h"
#include "filesystem.h"
#include "generation.h"
#include "interrupt.h"
#include "layout.h"
#include "package.h"
#include "package_catalog.h"
#include "package_transaction.h"
#include "release_metadata.h"
#include "runtime_journal.h"
#include "state.h"
#include "system.h"
#include "tool_preferences.h"
#include "uninstall_helper.h"
#include "uninstall_journal.h"
#include "update_journal.h"
#include "wrappers.h"
#include "unity.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char events[256];
static size_t event_count;
static RuntimeJournalKind journal_kind;
static CupError journal_detect_result;
static CupError package_tx_load_result;
static PackageTransactionStatus package_tx_status;
static CupError package_tx_recover_result;
static CupError generation_tx_load_result;
static UpdateJournalStatus generation_tx_status;
static CupError generation_tx_recover_result;
static CupError uninstall_load_result;
static UninstallJournalStatus uninstall_status;
static CupError uninstall_recover_result;
static CupError uninstall_helper_result;
static CupError scan_result;
static int scan_complete;
static PackageIdentity scanned[4];
static size_t scanned_count;
static PackageIssue scan_issues[2];
static size_t scan_issue_count;
static CupError quarantine_result;
static CupError state_load_result;
static StateFileStatus state_file_status;
static CupState scenario_state;
static CupError state_validate_result;
static CupError state_measure_result;
static CupError state_save_result;
static int state_path_exists;
static SystemPathKind state_path_kind;
static int backup_calls;
static int state_save_calls;
static int quarantine_calls;
static CupError preferences_result;
static int preferences_path_exists;
static CupError wrapper_build_result;
static CupError wrapper_apply_result;
static CupError staging_cleanup_result;
static GenerationInspection generation_inspection;
static CupError generation_inspect_result;
static int generation_has_assets;
static CupError release_load_result;
static CupError catalog_load_result;
static int catalog_path_exists;
static CupError root_kind_result;
static SystemPathKind root_kind;
static CupError lock_kind_result;
static SystemPathKind lock_kind;
static CupError lock_acquire_result;
static int lock_release_calls;
static CupError ensure_root_result;
static CupError ensure_runtime_result;
static int interrupt_fail_call;
static int interrupt_calls;
static CupError host_result;

static void event(char value) {
    if (event_count + 1 < sizeof(events)) {
        events[event_count++] = value;
        events[event_count] = '\0';
    }
}

static void identity_init(PackageIdentity *identity, const char *version) {
    memset(identity, 0, sizeof(*identity));
    strcpy(identity->component, "compiler");
    strcpy(identity->tool, "clang");
    strcpy(identity->host_platform, "linux-x64");
    strcpy(identity->target_platform, "linux-x64");
    strcpy(identity->version, version);
}

static void state_copy(CupState *destination, const CupState *source) {
    state_init(destination);
    if (source->installed_count != 0) {
        destination->installed = calloc(source->installed_count, sizeof(*destination->installed));
        TEST_ASSERT_NOT_NULL(destination->installed);
        memcpy(destination->installed,
               source->installed,
               source->installed_count * sizeof(*destination->installed));
        destination->installed_count = source->installed_count;
        destination->installed_capacity = source->installed_count;
    }
    memcpy(destination->defaults, source->defaults, sizeof(destination->defaults));
    destination->default_count = source->default_count;
}

void setUp(void) {
    state_free(&scenario_state);
    state_init(&scenario_state);
    events[0] = '\0'; event_count = 0;
    journal_kind = RUNTIME_JOURNAL_MISSING;
    journal_detect_result = CUP_OK;
    package_tx_load_result = CUP_OK; package_tx_status = PACKAGE_TRANSACTION_LOADED;
    package_tx_recover_result = CUP_OK;
    generation_tx_load_result = CUP_OK; generation_tx_status = CUP_UPDATE_JOURNAL_LOADED;
    generation_tx_recover_result = CUP_OK;
    uninstall_load_result = CUP_OK; uninstall_status = UNINSTALL_JOURNAL_LOADED;
    uninstall_recover_result = CUP_OK; uninstall_helper_result = CUP_OK;
    scan_result = CUP_OK; scan_complete = 1; scanned_count = 0; scan_issue_count = 0;
    quarantine_result = CUP_OK; quarantine_calls = 0;
    state_load_result = CUP_OK; state_file_status = STATE_FILE_LOADED;
    state_validate_result = CUP_OK; state_measure_result = CUP_OK; state_save_result = CUP_OK;
    state_path_exists = 1; state_path_kind = SYSTEM_PATH_REGULAR_FILE;
    backup_calls = 0; state_save_calls = 0;
    preferences_result = CUP_OK; preferences_path_exists = 1;
    wrapper_build_result = CUP_OK; wrapper_apply_result = CUP_OK;
    staging_cleanup_result = CUP_OK;
    memset(&generation_inspection, 0, sizeof(generation_inspection));
    generation_inspect_result = CUP_OK; generation_has_assets = 0;
    release_load_result = CUP_ERR_VALIDATION;
    catalog_load_result = CUP_OK; catalog_path_exists = 1;
    root_kind_result = CUP_OK; root_kind = SYSTEM_PATH_DIRECTORY;
    lock_kind_result = CUP_OK; lock_kind = SYSTEM_PATH_REGULAR_FILE;
    lock_acquire_result = CUP_OK; lock_release_calls = 0;
    ensure_root_result = CUP_OK; ensure_runtime_result = CUP_OK;
    interrupt_fail_call = 0; interrupt_calls = 0; host_result = CUP_OK;
}

void tearDown(void) { state_free(&scenario_state); }

/* Interrupt/platform/layout/lock boundaries. */
CupError interrupt_safe_point(void) { event('i'); interrupt_calls++; return interrupt_fail_call == interrupt_calls ? CUP_ERR_INTERRUPT : CUP_OK; }
CupError platform_get_host(char *buffer, size_t size) { if (host_result != CUP_OK) return host_result; snprintf(buffer,size,"linux-x64"); return CUP_OK; }
CupError layout_get_root(char *b,size_t s){ snprintf(b,s,"/root/.cup"); return CUP_OK; }
CupError layout_get_lock_path(char *b,size_t s){ snprintf(b,s,"/root/.cup/cup.lock"); return CUP_OK; }
CupError layout_get_state_path(char *b,size_t s){ snprintf(b,s,"/root/.cup/config/state.txt"); return CUP_OK; }
CupError layout_get_preferences_path(char *b,size_t s){ snprintf(b,s,"/root/.cup/config/preferences.txt"); return CUP_OK; }
CupError layout_get_package_catalog_path(char *b,size_t s){ snprintf(b,s,"/root/.cup/config/catalog.cfg"); return CUP_OK; }
CupError layout_get_staging_dir(char *b,size_t s){ snprintf(b,s,"/root/.cup/staging"); return CUP_OK; }
CupError layout_get_transaction_path(char *b,size_t s){ snprintf(b,s,"/root/.cup/transaction.txt"); return CUP_OK; }
CupError layout_root_snapshot_validate(void){ return CUP_OK; }
CupError layout_ensure_root(void){ return ensure_root_result; }
CupError layout_ensure_runtime(void){ event('R'); return ensure_runtime_result; }
CupError system_get_path_kind(const char *path, SystemPathKind *kind) {
    if (strstr(path,"cup.lock")) { if (lock_kind_result != CUP_OK) return lock_kind_result; *kind=lock_kind; return CUP_OK; }
    if (strstr(path,"state.txt")) { *kind=state_path_exists ? state_path_kind : SYSTEM_PATH_MISSING; return CUP_OK; }
    if (strstr(path,"preferences.txt")) { *kind=preferences_path_exists ? SYSTEM_PATH_REGULAR_FILE : SYSTEM_PATH_MISSING; return CUP_OK; }
    if (strstr(path,"catalog.cfg")) { *kind=catalog_path_exists ? SYSTEM_PATH_REGULAR_FILE : SYSTEM_PATH_MISSING; return CUP_OK; }
    if (strstr(path,"/root/.cup") && strcmp(path,"/root/.cup")==0) { if (root_kind_result != CUP_OK) return root_kind_result; *kind=root_kind; return CUP_OK; }
    *kind=SYSTEM_PATH_REGULAR_FILE; return CUP_OK;
}
CupError system_get_path_identity(const char *path,SystemPathIdentity *id){ (void)path; memset(id,0,sizeof(*id)); id->valid=1; id->kind=SYSTEM_PATH_REGULAR_FILE; return CUP_OK; }
CupError system_lock_acquire(SystemLock *lock,const char *path,SystemLockMode mode){ (void)path; event('L'); if(lock_acquire_result==CUP_OK){ lock->mode=mode; lock->active=1; } return lock_acquire_result; }
void system_lock_release(SystemLock *lock){ event('l'); lock->active=0; lock_release_calls++; }
CupError filesystem_backup_invalid(const char *p,char *out,size_t s){ (void)p; event('B'); backup_calls++; snprintf(out,s,"/recovery/invalid"); return CUP_OK; }
CupError filesystem_backup_invalid_if_identity(const char *p,const SystemPathIdentity *id,char *out,size_t s){ (void)p;(void)id; return filesystem_backup_invalid(p,out,s); }
CupError filesystem_clear_directory(const char *d,const char *keep){ (void)d;(void)keep; event('c'); return staging_cleanup_result; }

/* Shared transaction owners. */
CupError runtime_journal_detect(RuntimeJournalKind *kind){ event('J'); *kind=journal_kind; return journal_detect_result; }
void package_transaction_init(PackageTransaction *t){ memset(t,0,sizeof(*t)); }
CupError package_transaction_load(PackageTransaction *t,PackageTransactionStatus *s){ event('T'); identity_init(&t->package,"1.0.0"); t->file_identity.valid=1; t->file_identity.kind=SYSTEM_PATH_REGULAR_FILE; *s=package_tx_status; return package_tx_load_result; }
CupError package_transaction_recover(const PackageTransaction *t,CupState *s){ (void)t;(void)s; event('t'); return package_tx_recover_result; }
void update_journal_init(UpdateJournal *j){ memset(j,0,sizeof(*j)); }
CupError update_journal_load(UpdateJournal *j,UpdateJournalStatus *s){ (void)j; event('G'); *s=generation_tx_status; return generation_tx_load_result; }
CupError update_journal_recover(const UpdateJournal *j,int *f){ (void)j;(void)f; event('g'); return generation_tx_recover_result; }
void uninstall_journal_init(UninstallJournal *j){ memset(j,0,sizeof(*j)); strcpy(j->token,"0123456789abcdef"); }
CupError uninstall_journal_load(UninstallJournal *j,UninstallJournalStatus *s){ uninstall_journal_init(j); event('U'); *s=uninstall_status; return uninstall_load_result; }
CupError uninstall_helper_remove_stale(const char *root,const char *token,const SystemLock *lock){ (void)root;(void)token;(void)lock; event('u'); return uninstall_helper_result; }
CupError uninstall_journal_recover(const UninstallJournal *j){ (void)j; event('v'); return uninstall_recover_result; }

/* Package scan/reconciliation. */
void package_list_init(PackageList *p){ memset(p,0,sizeof(*p)); p->complete=1; }
void package_list_free(PackageList *p){ free(p->items); memset(p,0,sizeof(*p)); }
CupError package_scan(PackageList *p,FILE *diag){ size_t i;(void)diag; event('P'); if(scan_result!=CUP_OK)return scan_result; package_list_free(p); package_list_init(p); p->complete=scan_complete; if(scanned_count){ p->items=calloc(scanned_count,sizeof(*p->items)); TEST_ASSERT_NOT_NULL(p->items); for(i=0;i<scanned_count;i++)p->items[i]=scanned[i]; p->count=p->capacity=p->total_count=scanned_count;} for(i=0;i<scan_issue_count;i++)p->issues[i]=scan_issues[i]; p->issue_count=p->total_issue_count=scan_issue_count; return CUP_OK; }
int package_list_contains(const PackageList *p,const PackageIdentity *x){ size_t i; for(i=0;i<p->count;i++) if(strcmp(p->items[i].component,x->component)==0&&strcmp(p->items[i].tool,x->tool)==0&&strcmp(p->items[i].target_platform,x->target_platform)==0&&strcmp(p->items[i].version,x->version)==0)return 1; return 0; }
const char *package_issue_reason_name(PackageIssueReason r){ (void)r; return "invalid"; }
CupError package_quarantine(const PackageIssue *i,char *out,size_t s){ (void)i; event('Q'); quarantine_calls++; snprintf(out,s,"/recovery/package"); return quarantine_result; }
CupError package_identity_validate(const PackageIdentity *i,FILE *d){ (void)d; return i&&i->component[0]&&i->tool[0]&&i->version[0]?CUP_OK:CUP_ERR_INVALID_INPUT; }
CupError package_identity_format_selector(const PackageIdentity *i,char *b,size_t s){ return snprintf(b,s,"%s@%s",i->tool,i->version)>=0?CUP_OK:CUP_ERR_BUFFER_TOO_SMALL; }
CupError package_identity_get_scope(const PackageIdentity *i,PackageScope *scope){ memset(scope,0,sizeof(*scope)); strcpy(scope->component,i->component); strcpy(scope->host_platform,i->host_platform); strcpy(scope->target_platform,i->target_platform); return CUP_OK; }

/* State owner. */
void state_init(CupState *s){ memset(s,0,sizeof(*s)); }
void state_free(CupState *s){ if(s){free(s->installed);memset(s,0,sizeof(*s));} }
CupError state_load(CupState *s,StateFileStatus *status,SystemPathIdentity *id,FILE *diag){ (void)diag; event('S'); *status=state_file_status; if(id){memset(id,0,sizeof(*id)); if(state_file_status==STATE_FILE_LOADED){id->valid=1;id->kind=SYSTEM_PATH_REGULAR_FILE;}} if(state_load_result==CUP_OK&&state_file_status==STATE_FILE_LOADED)state_copy(s,&scenario_state); return state_load_result; }
CupError state_validate(const CupState *s,FILE *diag){ (void)s;(void)diag; return state_validate_result; }
CupError state_measure_persistent(const CupState *s,size_t *size){ (void)s; event('M'); if(size)*size=128; return state_measure_result; }
CupError state_save(const CupState *s,const SystemPathIdentity *e,SystemPathIdentity *p){ (void)s;(void)e;(void)p; event('W'); state_save_calls++; return state_save_result; }
int state_find_installed(const CupState *s,const PackageIdentity *i){ size_t n; for(n=0;n<s->installed_count;n++) if(strcmp(s->installed[n].tool,i->tool)==0&&strcmp(s->installed[n].version,i->version)==0&&strcmp(s->installed[n].target_platform,i->target_platform)==0)return (int)n; return -1; }
CupError state_add_installed(CupState *s,const PackageIdentity *i){ PackageIdentity *v=realloc(s->installed,(s->installed_count+1)*sizeof(*v)); if(!v)return CUP_ERR_TEMPORARY; s->installed=v;s->installed[s->installed_count++]=*i;s->installed_capacity=s->installed_count;return CUP_OK; }
CupError state_remove_installed(CupState *s,const PackageIdentity *i){ int n=state_find_installed(s,i); if(n<0)return CUP_ERR_NOT_INSTALLED; if((size_t)n+1<s->installed_count)memmove(&s->installed[n],&s->installed[n+1],(s->installed_count-(size_t)n-1)*sizeof(*s->installed));s->installed_count--;return CUP_OK; }
CupError state_clear_matching_default(CupState *s,const PackageIdentity *i){ size_t n=0; while(n<s->default_count){ if(strcmp(s->defaults[n].tool,i->tool)==0&&strcmp(s->defaults[n].version,i->version)==0){ if(n+1<s->default_count)memmove(&s->defaults[n],&s->defaults[n+1],(s->default_count-n-1)*sizeof(s->defaults[0]));s->default_count--; } else n++; } return CUP_OK; }
CupError state_clear_default(CupState *s,const PackageScope *scope){ size_t n; for(n=0;n<s->default_count;n++){ if(strcmp(s->defaults[n].component,scope->component)==0&&strcmp(s->defaults[n].target_platform,scope->target_platform)==0){ if(n+1<s->default_count)memmove(&s->defaults[n],&s->defaults[n+1],(s->default_count-n-1)*sizeof(s->defaults[0]));s->default_count--;return CUP_OK; }} return CUP_OK; }

/* Preferences/wrappers. */
void tool_preferences_init(ToolPreferences *p){ memset(p,0,sizeof(*p)); }
CupError tool_preferences_load(ToolPreferences *p, FILE *diagnostics){ (void)diagnostics; (void)p; event('F'); return preferences_result; }
void wrapper_plan_init(WrapperPlan *p){ memset(p,0,sizeof(*p)); }
void wrapper_plan_free(WrapperPlan *p){ free(p->items); memset(p,0,sizeof(*p)); }
CupError wrapper_plan_build(WrapperPlan *p,const CupState *s){ (void)p;(void)s; event('D'); return wrapper_build_result; }
CupError wrapper_plan_apply(const WrapperPlan *p){ (void)p; event('d'); return wrapper_apply_result; }

/* Generation/release boundaries. Development-success normally has no installed generation. */
void release_metadata_init(ReleaseMetadata *m){ memset(m,0,sizeof(*m)); }
void release_metadata_free(ReleaseMetadata *m){ free(m->assets); memset(m,0,sizeof(*m)); }
CupError release_metadata_load(const char *p,ReleaseMetadata *m){ (void)p;(void)m; event('E'); return release_load_result; }
const ReleaseAsset *release_metadata_find_asset(const ReleaseMetadata *m,const char *name){ (void)m;(void)name; return NULL; }
CupError generation_inspect(GenerationInspection *i){ event('N'); *i=generation_inspection; return generation_inspect_result; }
int generation_has_installed_assets(const GenerationInspection *i){ (void)i; return generation_has_assets; }
int generation_installed_is_valid(const GenerationInspection *i){ (void)i; return 0; }
CupError generation_asset_spec(GenerationAssetId id,GenerationAssetSpec *s){ memset(s,0,sizeof(*s));s->id=id;snprintf(s->destination,sizeof(s->destination),"/root/.cup/asset-%d",(int)id);snprintf(s->release_name,sizeof(s->release_name),"asset-%d",(int)id);return CUP_OK; }
const ReleaseAsset *generation_manifest_asset(const ReleaseMetadata *m,GenerationAssetId id){ (void)m;(void)id; return NULL; }
CupError generation_validate_manifest(const ReleaseMetadata *m){ (void)m; return CUP_OK; }
CupError checksum_sha256_file(const char *p,char *h,size_t s){ (void)p; if(s<65)return CUP_ERR_BUFFER_TOO_SMALL; memset(h,'a',64);h[64]='\0';return CUP_OK; }
CupError system_is_executable(const char *p,int *v){ (void)p;*v=1;return CUP_OK; }
CupError system_set_executable(const char *p,int v){ (void)p;(void)v;return CUP_OK; }
CupError system_is_read_only(const char *p,int *v){ (void)p;*v=1;return CUP_OK; }
CupError system_set_read_only(const char *p,int v){ (void)p;(void)v;return CUP_OK; }

/* Catalog boundary. */
void package_catalog_init(PackageCatalog *c){ memset(c,0,sizeof(*c)); }
void package_catalog_free(PackageCatalog *c){ free(c->packages); memset(c,0,sizeof(*c)); }
CupError package_catalog_load_installed(PackageCatalog *c){ (void)c; event('C'); return catalog_load_result; }

static void assert_success_order(void) {
    const char *j = strchr(events,'J');
    const char *p = strchr(events,'P');
    const char *s = strchr(events,'S');
    const char *m = strchr(events,'M');
    const char *f = strchr(events,'F');
    const char *d = strchr(events,'D');
    const char *c = strchr(events,'c');
    const char *n = strchr(events,'N');
    const char *cat = strchr(events,'C');
    TEST_ASSERT_NOT_NULL(j); TEST_ASSERT_NOT_NULL(p); TEST_ASSERT_NOT_NULL(s); TEST_ASSERT_NOT_NULL(m);
    TEST_ASSERT_NOT_NULL(f); TEST_ASSERT_NOT_NULL(d); TEST_ASSERT_NOT_NULL(c); TEST_ASSERT_NOT_NULL(n); TEST_ASSERT_NOT_NULL(cat);
    TEST_ASSERT_TRUE(j < p && p < s && s < m && m < f && f < d && d < c && c < n && n < cat);
}

static void test_success_uses_frozen_phase_order(void) {
    TEST_ASSERT_EQUAL_INT(CUP_OK, command_repair());
    assert_success_order();
    TEST_ASSERT_EQUAL_INT(1, lock_release_calls);
}

static void test_pending_package_transaction_requires_valid_state_before_scan(void) {
    journal_kind=RUNTIME_JOURNAL_PACKAGE;
    state_load_result=CUP_ERR_STATE_LOAD;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_TRANSACTION, command_repair());
    TEST_ASSERT_NOT_NULL(strchr(events,'T'));
    TEST_ASSERT_NULL(strchr(events,'P'));
    TEST_ASSERT_EQUAL_INT(0, backup_calls);
}

static void test_pending_package_transaction_recovers_before_scan(void) {
    journal_kind=RUNTIME_JOURNAL_PACKAGE;
    TEST_ASSERT_EQUAL_INT(CUP_OK, command_repair());
    TEST_ASSERT_TRUE(strchr(events,'t') < strchr(events,'P'));
}

static void test_generation_transaction_recovers_before_scan(void) {
    journal_kind=RUNTIME_JOURNAL_GENERATION;
    TEST_ASSERT_EQUAL_INT(CUP_OK, command_repair());
    TEST_ASSERT_TRUE(strchr(events,'g') < strchr(events,'P'));
}

static void test_uninstall_transaction_recovers_before_scan(void) {
    const char *loaded;
    const char *helper;
    const char *recovered;
    const char *scan;

    journal_kind=RUNTIME_JOURNAL_UNINSTALL;
    TEST_ASSERT_EQUAL_INT(CUP_OK, command_repair());
    loaded = strchr(events,'U');
    helper = strchr(events,'u');
    recovered = strchr(events,'v');
    scan = strchr(events,'P');
    TEST_ASSERT_NOT_NULL(loaded);
    TEST_ASSERT_NOT_NULL(helper);
    TEST_ASSERT_NOT_NULL(recovered);
    TEST_ASSERT_NOT_NULL(scan);
    TEST_ASSERT_TRUE(loaded < helper && helper < recovered && recovered < scan);
}

static void test_ambiguous_journal_stops_before_scan(void) {
    journal_detect_result=CUP_ERR_TRANSACTION;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_TRANSACTION, command_repair());
    TEST_ASSERT_NULL(strchr(events,'P'));
}

static void test_incomplete_scan_stops_before_state_mutation(void) {
    scan_complete=0;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INCONSISTENT_STATE, command_repair());
    TEST_ASSERT_NULL(strchr(events,'S'));
    TEST_ASSERT_EQUAL_INT(0, quarantine_calls);
    TEST_ASSERT_EQUAL_INT(0, state_save_calls);
}

static void test_state_budget_preflight_happens_before_quarantine_and_backup(void) {
    state_load_result=CUP_ERR_STATE_LOAD;
    state_measure_result=CUP_ERR_STATE_FULL;
    state_path_kind=SYSTEM_PATH_REGULAR_FILE;
    scan_issue_count=1; scan_issues[0].can_quarantine=1;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_STATE_FULL, command_repair());
    TEST_ASSERT_EQUAL_INT(0, quarantine_calls);
    TEST_ASSERT_EQUAL_INT(0, backup_calls);
    TEST_ASSERT_EQUAL_INT(0, state_save_calls);
}

static void test_invalid_state_is_preserved_then_reconstructed(void) {
    state_load_result=CUP_ERR_STATE_LOAD;
    state_path_kind=SYSTEM_PATH_REGULAR_FILE;
    identity_init(&scanned[0],"2.0.0"); scanned_count=1;
    TEST_ASSERT_EQUAL_INT(CUP_OK, command_repair());
    TEST_ASSERT_EQUAL_INT(1, backup_calls);
    TEST_ASSERT_EQUAL_INT(1, state_save_calls);
    TEST_ASSERT_TRUE(strchr(events,'B') < strchr(events,'W'));
}

static void test_invalid_preferences_preserved_before_wrappers(void) {
    preferences_result=CUP_ERR_VALIDATION;
    TEST_ASSERT_EQUAL_INT(CUP_OK, command_repair());
    TEST_ASSERT_EQUAL_INT(1, backup_calls);
    TEST_ASSERT_TRUE(strchr(events,'F') < strchr(events,'B'));
    TEST_ASSERT_TRUE(strchr(events,'B') < strchr(events,'D'));
}

static void test_future_catalog_is_preserved_and_not_seeded(void) {
    catalog_load_result=CUP_ERR_NOT_AVAILABLE;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_NOT_AVAILABLE, command_repair());
    TEST_ASSERT_EQUAL_INT(0, backup_calls);
}

static void test_malformed_catalog_is_preserved_but_not_reseeded_in_development(void) {
    catalog_load_result=CUP_ERR_CATALOG;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_CATALOG, command_repair());
    TEST_ASSERT_EQUAL_INT(1, backup_calls);
    TEST_ASSERT_TRUE(strchr(events,'C') < strchr(events,'B'));
}

static void test_partial_installed_generation_is_not_masked_by_development_mode(void) {
    generation_has_assets=1;
    release_load_result=CUP_ERR_VALIDATION;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_NOT_AVAILABLE, command_repair());
    TEST_ASSERT_NOT_NULL(strchr(events,'N'));
    TEST_ASSERT_NOT_NULL(strchr(events,'E'));
    TEST_ASSERT_NULL(strchr(events,'C'));
}

static void test_root_missing_does_not_initialize_runtime(void) {
    root_kind=SYSTEM_PATH_MISSING;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_NOT_INSTALLED, command_repair());
    TEST_ASSERT_NULL(strchr(events,'R'));
}

int main(void) {
    state_init(&scenario_state);
    UNITY_BEGIN();
    RUN_TEST(test_success_uses_frozen_phase_order);
    RUN_TEST(test_pending_package_transaction_requires_valid_state_before_scan);
    RUN_TEST(test_pending_package_transaction_recovers_before_scan);
    RUN_TEST(test_generation_transaction_recovers_before_scan);
    RUN_TEST(test_uninstall_transaction_recovers_before_scan);
    RUN_TEST(test_ambiguous_journal_stops_before_scan);
    RUN_TEST(test_incomplete_scan_stops_before_state_mutation);
    RUN_TEST(test_state_budget_preflight_happens_before_quarantine_and_backup);
    RUN_TEST(test_invalid_state_is_preserved_then_reconstructed);
    RUN_TEST(test_invalid_preferences_preserved_before_wrappers);
    RUN_TEST(test_future_catalog_is_preserved_and_not_seeded);
    RUN_TEST(test_malformed_catalog_is_preserved_but_not_reseeded_in_development);
    RUN_TEST(test_partial_installed_generation_is_not_masked_by_development_mode);
    RUN_TEST(test_root_missing_does_not_initialize_runtime);
    return UNITY_END();
}
