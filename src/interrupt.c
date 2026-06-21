#include "interrupt.h"

#include <signal.h>
#include <stddef.h>
#if defined(_WIN32)
#include <windows.h>
#endif

// INTERRUPT STATE
static volatile sig_atomic_t g_interrupted = 0;
static int g_handler_active = 0;

// PLATFORM HANDLERS
#if defined(_WIN32)
static BOOL WINAPI handle_console_event(DWORD type) {
    switch (type) {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
        case CTRL_CLOSE_EVENT:
            g_interrupted = 1;
            return TRUE;
        default:
            return FALSE;
    }
}
#else
static void handle_sigint(int sig) {
    (void)sig;
    g_interrupted = 1;
}
#endif

// PUBLIC API
void interrupt_setup(void) {
    g_interrupted = 0;
#if defined(_WIN32)
    g_handler_active = SetConsoleCtrlHandler(handle_console_event, TRUE) != 0;
#else
    struct sigaction action;
    action.sa_handler = handle_sigint;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;
    g_handler_active = sigaction(SIGINT, &action, NULL) == 0;
#endif
}

void interrupt_reset(void) {
#if defined(_WIN32)
    if (g_handler_active) {
        SetConsoleCtrlHandler(handle_console_event, FALSE);
    }
#else
    if (g_handler_active) {
        struct sigaction action;
        action.sa_handler = SIG_DFL;
        sigemptyset(&action.sa_mask);
        action.sa_flags = 0;
        sigaction(SIGINT, &action, NULL);
    }
#endif
    g_handler_active = 0;
    g_interrupted = 0;
}

int interrupt_requested(void) {
    return g_interrupted != 0;
}
