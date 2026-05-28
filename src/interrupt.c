#include "interrupt.h"

#include <signal.h>
#ifdef _WIN32
#include <windows.h>
#endif

static volatile sig_atomic_t g_interrupted = 0;

#ifdef _WIN32
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

void interrupt_setup(void) {
    g_interrupted = 0;
#ifdef _WIN32
    SetConsoleCtrlHandler(handle_console_event, TRUE);
#else
    signal(SIGINT, handle_sigint);
#endif
}

void interrupt_reset(void) {
    g_interrupted = 0;
}

int interrupt_requested(void) {
    return g_interrupted != 0;
}