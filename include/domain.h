#ifndef CUP_DOMAIN_H
#define CUP_DOMAIN_H

/* Small closed-domain types shared by state, package, wrapper, and CLI boundaries. */

#include "constants.h"

/* Component + host + target uniquely identify one selectable package scope. */
typedef struct {
    char component[MAX_IDENTIFIER_LEN];
    char host_platform[MAX_PLATFORM_LEN];
    char target_platform[MAX_PLATFORM_LEN];
} ScopeKey;

/* Concrete releases exclude symbolic selectors such as stable. */
typedef char ConcreteRelease[MAX_IDENTIFIER_LEN];

/* Worst-case public wrapper name includes target prefix and Windows .cmd suffix. */
typedef char PublicCommandName[MAX_PUBLIC_COMMAND_NAME_LEN];

#endif /* CUP_DOMAIN_H */
