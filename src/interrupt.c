/*
 * Owns the native interrupt-handler lifecycle for one mutating public command and exposes only
 * an async-signal-safe request flag.
 */

#include "interrupt.h"

#include <signal.h>
#include <stddef.h>
#if defined(_WIN32)
#include <windows.h>
#endif

static volatile sig_atomic_t g_interrupted = 0;
static int g_handler_active = 0;

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
static struct sigaction g_previous_sigint;
static struct sigaction g_previous_sigterm;
static int g_sigint_saved = 0;
static int g_sigterm_saved = 0;

static void handle_signal(int signal_number) {
    (void)signal_number;
    g_interrupted = 1;
}
#endif

/* Handler lifecycle. Installation failure is observable, and the previous process handlers are
 * restored on disable. */
CupError interrupt_enable(void) {
    if (g_handler_active) {
        return CUP_ERR_INVALID_INPUT;
    }

    g_interrupted = 0;
#if defined(_WIN32)
    if (!SetConsoleCtrlHandler(handle_console_event, TRUE)) {
        return CUP_ERR_FILESYSTEM;
    }
#else
    {
        struct sigaction action;

        action.sa_handler = handle_signal;
        sigemptyset(&action.sa_mask);
        action.sa_flags = 0;
        if (sigaction(SIGINT, &action, &g_previous_sigint) != 0) {
            return CUP_ERR_FILESYSTEM;
        }
        g_sigint_saved = 1;
        if (sigaction(SIGTERM, &action, &g_previous_sigterm) != 0) {
            sigaction(SIGINT, &g_previous_sigint, NULL);
            g_sigint_saved = 0;
            return CUP_ERR_FILESYSTEM;
        }
        g_sigterm_saved = 1;
    }
#endif
    g_handler_active = 1;
    return CUP_OK;
}

void interrupt_disable(void) {
    if (!g_handler_active) {
        g_interrupted = 0;
        return;
    }

#if defined(_WIN32)
    SetConsoleCtrlHandler(handle_console_event, FALSE);
#else
    if (g_sigterm_saved) {
        sigaction(SIGTERM, &g_previous_sigterm, NULL);
    }
    if (g_sigint_saved) {
        sigaction(SIGINT, &g_previous_sigint, NULL);
    }
    g_sigint_saved = 0;
    g_sigterm_saved = 0;
#endif
    g_handler_active = 0;
    g_interrupted = 0;
}

void interrupt_clear(void) {
    g_interrupted = 0;
}

int interrupt_requested(void) {
    return g_interrupted != 0;
}
