/*
 * Owns the native interrupt-handler lifecycle for one state-changing public command and exposes
 * only an async-signal-safe request flag.
 */

#include "interrupt.h"

#include <signal.h>
#include <stddef.h>
#if defined(_WIN32)
#include <windows.h>
#endif

#if defined(_WIN32)
static volatile LONG g_interrupted = 0;
#else
static volatile sig_atomic_t g_interrupted = 0;
#endif
#if defined(_WIN32)
static BOOL WINAPI handle_console_event(DWORD type) {
    switch (type) {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
        case CTRL_CLOSE_EVENT:
            InterlockedExchange(&g_interrupted, 1);
            return TRUE;
        default:
            return FALSE;
    }
}
#else
static void handle_signal(int signal_number) {
    (void)signal_number;
    g_interrupted = 1;
}
#endif

/* Install handlers once for the remaining lifetime of this CLI process. */
CupError interrupt_enable(void) {
#if defined(_WIN32)
    InterlockedExchange(&g_interrupted, 0);
    return SetConsoleCtrlHandler(handle_console_event, TRUE) ? CUP_OK : CUP_ERR_FILESYSTEM;
#else
    struct sigaction action = {0};

    g_interrupted = 0;
    action.sa_handler = handle_signal;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;
    if (sigaction(SIGINT, &action, NULL) != 0 || sigaction(SIGTERM, &action, NULL) != 0) {
        return CUP_ERR_FILESYSTEM;
    }
    return CUP_OK;
#endif
}

int interrupt_requested(void) {
#if defined(_WIN32)
    return InterlockedCompareExchange(&g_interrupted, 0, 0) != 0;
#else
    return g_interrupted != 0;
#endif
}

CupError interrupt_safe_point(void) {
    return interrupt_requested() ? CUP_ERR_INTERRUPT : CUP_OK;
}
