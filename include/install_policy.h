#ifndef CUP_INSTALL_POLICY_H
#define CUP_INSTALL_POLICY_H

/*
 * Official scoped defaults, profiles and curated toolchains compiled into this cup generation.
 */

#include <stddef.h>

#include "constants.h"
#include "error.h"
#include "package.h"

typedef struct {
    PackageScope scope;
    char tool[MAX_IDENTIFIER_LEN];
} InstallDefault;

typedef struct {
    char name[MAX_IDENTIFIER_LEN];
    char items[MAX_INSTALL_LIST_ITEMS][MAX_IDENTIFIER_LEN];
    size_t item_count;
} InstallNamedList;

typedef struct {
    InstallDefault defaults[MAX_INSTALL_DEFAULTS];
    size_t default_count;
    InstallNamedList profiles[MAX_INSTALL_PROFILES];
    size_t profile_count;
    InstallNamedList toolchains[MAX_INSTALL_TOOLCHAINS];
    size_t toolchain_count;
} InstallPolicy;

/* Load one complete immutable policy. Success replaces the model; failure leaves it empty.
 * Query functions require a successfully loaded, unmodified model. */
void install_policy_init(InstallPolicy *policy);
CupError install_policy_load(InstallPolicy *policy);

/* Query immutable defaults and named plans after a successful load. */
const InstallDefault *install_policy_find_default(const InstallPolicy *policy,
                                                  const char *host_platform,
                                                  const char *target_platform,
                                                  const char *component);
const InstallNamedList *install_policy_find_profile(const InstallPolicy *policy, const char *name);
const InstallNamedList *install_policy_find_toolchain(const InstallPolicy *policy,
                                                      const char *name);

#endif /* CUP_INSTALL_POLICY_H */
