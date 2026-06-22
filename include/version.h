#ifndef CUP_VERSION_H
#define CUP_VERSION_H

#define CUP_VERSION_BASE "0.1.0"

#if defined(CUP_BUILD_RELEASE)
#define CUP_VERSION CUP_VERSION_BASE
#else
#define CUP_VERSION CUP_VERSION_BASE "-dev"
#endif

#endif /* CUP_VERSION_H */
